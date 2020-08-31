
#include <shared_mutex>
#include <tuple>
#include <eosio/chain_plugin/chain_plugin.hpp>
#include <eosio/http_plugin/http_plugin.hpp>
#include <eosio/chain/controller.hpp> 
#include <eosio/chain/thread_utils.hpp>
#include <eosio/query_api_plugin/query_api_plugin.hpp>
#include <fc/io/json.hpp>

namespace eosio 
{

static appbase::abstract_plugin& _query_api_plugin = app().register_plugin<query_api_plugin>();

using namespace eosio;
using namespace std;

namespace
{
   template <typename T>
   T parse_body( const string &body )
   {
      if ( body.empty() )
      {
         EOS_THROW( invalid_http_request, "A Request body is required" );
      }

      try
      {
         try
         {
            return fc::json::from_string(body).as<T>();
         }
         catch ( const chain_exception &e )
         {
            throw fc::exception( e );
         }
      }
      EOS_RETHROW_EXCEPTIONS( chain::invalid_http_request, "Unable to parse valid input from POST body" );
   }

   bool valid_token_contract( const chain_apis::read_only &ro, const action &act )
   {
      if ( act.name == N(transfer) )
      {
         const auto result = ro.get_abi( chain_apis::read_only::get_abi_params { act.account } );
         if ( result.abi )
         {
            return any_of( result.abi->tables.begin(), result.abi->tables.end(), [](const auto &v) {
               return v.name == N(accounts);
            });
         }
      }
      return false;
   }
}

namespace io_params
{
   struct get_account_tokens_params
   {
      name account_name;
   };

   struct get_account_tokens_result
   {
      struct code_assets
      {
         name          code;
         vector<asset> assets;
      };

      vector<code_assets> tokens;
   };
}

class query_api_plugin_impl
{
   controller &_ctrl;
   chain_plugin &_chain_plugin;
   shared_mutex _smutex;
   unordered_set<account_name> _token_accounts;
   named_thread_pool _thread_pool;
   uint8_t _thread_num;
   fc::optional<boost::signals2::scoped_connection> _accepted_transaction_connection;

public:
   static auto register_apis( query_api_plugin_impl &impl )
   {
      return api_description {
         {
            "/v1/query/get_token_contracts",
            [&] (string, string, url_response_callback cb) { return impl.get_token_contracts(move(cb)); }
         },
         {
            "/v1/query/get_account_tokens",
            [&] (string, string body, url_response_callback cb) { return impl.get_account_tokens(move(body), move(cb)); }
         }
      };
   }

public:
   query_api_plugin_impl( chain_plugin &chain, uint8_t thread_num, const unordered_set<account_name> &&accounts )
      : _ctrl( chain.chain() )
      , _chain_plugin( chain )
      , _token_accounts( accounts )
      , _thread_num( thread_num )
      , _thread_pool( "query", static_cast<size_t>(thread_num) )
   {}

   void initialize( uint32_t min_block, uint32_t max_block )
   {
      const auto &blog = _ctrl.block_log();
      auto first_block_num = std::max<uint32_t>( blog.first_block_num(), min_block );
      auto head_block_num = std::min<uint32_t>( blog.head()->block_num(), max_block );
      if ( first_block_num > head_block_num )
      {
         return;
      }

      ilog( "scanning token accounts from block ${b} to block ${e} in block_log, this may take significant minutes.", ("b", first_block_num)("e", head_block_num) );
      for ( auto i = first_block_num; i <= head_block_num; ++i )
      {
         const signed_block_ptr &block = blog.read_block_by_num( i );
         for_each( block->transactions.begin(), block->transactions.end(), [&](const auto &v)
         {
            if ( v.trx.template contains<packed_transaction>() )
            {
               const auto &tx = v.trx.template get<packed_transaction>().get_transaction();
               for_each( tx.actions.begin(), tx.actions.end(), [&](const auto &a)
               {
                  if ( valid_token_contract(_chain_plugin.get_read_only_api(), a) )
                  {
                     _token_accounts.insert( a.account );
                  }
               });
            }
         });
         if ( (i - first_block_num) % 5000 == 0 )
         {
            ilog( "have filtered ${n} token accounts so far from 5000 blocks (${i} of ${e}) in block_log", ("n", _token_accounts.size())("i", i)("e", head_block_num) );
         }
      }

      ilog( "scanning done! have totally filtered ${n} token accounts from ${b} blocks in block_log", ("n", _token_accounts.size())("b", head_block_num - first_block_num + 1) );
   }

   void startup()
   {
      _accepted_transaction_connection.emplace(
         _ctrl.accepted_transaction.connect( [&](const transaction_metadata_ptr &tm) {
            update_token_accounts( tm );
         })
      );
   }

   void shutdown()
   {
      _accepted_transaction_connection.reset();
   }

   void update_token_accounts( const transaction_metadata_ptr &tx_meta )
   {
      const auto &tx = tx_meta->packed_trx()->get_transaction();
      unordered_set<account_name> addons;
      for_each( tx.actions.begin(), tx.actions.end(), [&](const auto &a)
      {
         if ( valid_token_contract(_chain_plugin.get_read_only_api(), a) && _token_accounts.count(a.account) <= 0 )
         {
            addons.insert( a.account );
         }
      });

      if (! addons.empty() )
      {
         unique_lock<shared_mutex> wl( _smutex );
         _token_accounts.insert( addons.begin(), addons.end() );
         ilog( "filtered ${n} new token accounts from transaction ${id}", ("n", addons.size())("id", tx_meta->id()) );
      }
   }

   //=========================
   // HTTP API implements
   //=========================

   void get_token_contracts( url_response_callback &&cb )
   {
      fc::variant result;
      shared_lock<shared_mutex> rl( _smutex );
      fc::to_variant( _token_accounts, result );
      rl.unlock();
      cb( 200, result );
   }

   void get_account_tokens( string &&body, url_response_callback &&cb )
   {
      vector<future> promises;
      for ( auto i = 0; i < _thread_num; ++i )
      {
         promises.emplace_back( async_thread_pool( _thread_pool.get_executor(), [i, step = _token_accounts.size() / _thread_num, this]()
         {
            auto params = parse_body<io_params::get_account_tokens_params>( body );
            chain_apis::read_only::get_currency_balance_params cb_params {
               .account = params.account_name
            };
            unordered_set<account_name> invalid;
            vector<io_params::get_account_tokens_result::code_assets> tokens;
            auto read_only = _chain_plugin.get_read_only_api();
            shared_lock<shared_mutex> rl( _smutex );
            for ( auto b = advance(_token_accounts.begin(), i * step), 
               e = advance(_token_accounts.begin(), (i + 1 < _thread_num ? (i + 1) * step : _token_accounts.size())); b != e; ++b )
            {
               cb_params.code = b->code;
               try
               {
                  vector<asset> assets = read_only.get_currency_balance( cb_params );
                  if (! assets.empty() )
                  {
                     tokens.emplace_back( io_params::get_account_tokens_result::code_assets {
                        .code   = cb_params.code,
                        .assets = assets
                     });
                  }
               }
               catch (...)
               {
                  // maybe the token contract in code has been removed with set_code()
                  invalid.insert( code );
               }
            }
            rl.unlock();
            return make_tuple( tokens, invalid );
         }));
      }

      unordered_set<account_name> total_invalid;
      io_params::get_account_tokens_result account_tokens;
      for ( const future &promise : promises )
      {
         auto [tokens, invalid] = promise.get();
         account_tokens.tokens.insert( tokens.begin(), tokens.end() );
         total_invalid.insert( invalid.begin(), invalid.end() );
      }

      if (! total_invalid.empty() )
      {
         unique_lock<shared_mutex> wl( _smutex );
         _token_accounts.erase( total_invalid.begin(), total_invalid.end() );
      }

      fc::variant result;
      fc::to_variant( account_tokens, result );
      cb( 200, result );
   }
};

query_api_plugin::query_api_plugin()
{
   app().register_config_type<uint8_t>();
}

// API plugin no need to do these
void query_api_plugin::set_program_options( options_description &cli, options_description &cfg )
{
   cfg.add_options()
      ("thread-pool-size", bpo::value<uint8_t>()->default_value(2), "number of threads in thread_pool.")
      ("blocknum-scan-from", bpo::value<uint32_t>()->default_value(0), "lower bound block number the scanning process scans from (can be lower than the minimum in block_log).")
      ("blocknum-scan-to", bpo::value<uint32_t>()->default_value(-1), "upper bound block number the scanning process scans to (can be greater than the maximum in block_blog).");

   cli.add_options()
      ("accounts-json", bpo::value<bfs::path>(), "the file path to import recorded token accounts.");
}

void query_api_plugin::plugin_initialize( const variables_map &options )
{
   ilog( "starting query_api_plugin" );

   auto pool_size = options.at("thread-pool-size").as<uint8_t>();
   EOS_ASSERT( pool_size > 0, plugin_config_exception, "invalid thread_pool size config (> 0)" );

   auto min_block = options.at("blocknum-scan-from").as<uint32_t>();
   auto max_block = options.at("blocknum-scan-to").as<uint32_t>();
   EOS_ASSERT( max_block >= min_block, plugin_config_exception, "invalid block number config (from >= to)" );

   unordered_set<account_name> accounts;
   if ( options.count("accounts-json") )
   {
      auto accounts_file = options.at("accounts-json").as<bfs::path>();
      if ( accounts_file.is_relative() )
      {
         accounts_file = bfs::current_path() / accounts_file;
      }
      EOS_ASSERT( fc::is_regular_file(accounts_file), plugin_config_exception,
         "specified accounts json file '${f}' does not exist.", ("f", accounts_file.generic_string()) );
      accounts = fc::json::from_file(accounts_file).as<unordered_set<account_name>>();
      ilog( "imported ${n} token accounts from '${f}'", ("n", accounts.size())("f", accounts_file.generic_string()) );
   }

   my.reset( new query_api_plugin_impl(app().get_plugin<chain_plugin>(), pool_size, move(accounts)) );
   my->initialize( min_block, max_block );
}

// set up API handler
void query_api_plugin::plugin_startup()
{
   app().get_plugin<http_plugin>().add_api( query_api_plugin_impl::register_apis(*my) );
   my->startup();
}

void query_api_plugin::plugin_shutdown()
{
   my->shutdown();
}

}

FC_REFLECT( eosio::io_params::get_account_tokens_params, (account_name) )
FC_REFLECT( eosio::io_params::get_account_tokens_result, (tokens) )
FC_REFLECT( eosio::io_params::get_account_tokens_result::code_assets, (code)(assets) )
