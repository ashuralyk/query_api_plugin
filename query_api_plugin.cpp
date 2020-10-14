
#include <shared_mutex>
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
         try
         {
            const auto result = ro.get_abi( chain_apis::read_only::get_abi_params { act.account } );
            if ( result.abi )
            {
               return any_of( result.abi->tables.begin(), result.abi->tables.end(), [](const auto &v) {
                  return v.name == N(accounts);
               });
            }
         }
         catch (...)
         {}
      }
      return false;
   }
}

namespace io_params
{
   struct get_account_tokens_params
   {
      name account_name;
      bool recent;
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
   unordered_set<account_name> _recent_token_accounts;
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
   query_api_plugin_impl( chain_plugin &chain, uint16_t thread_num )
      : _ctrl( chain.chain() )
      , _chain_plugin( chain )
      , _thread_pool( "query", static_cast<size_t>(thread_num) )
      , _thread_num( thread_num )
   {}

   void initialize()
   {
      const auto &accounts = _ctrl.db().get_index<chain::account_index>().indices().get<by_id>();
      ilog( "start scanning whole ${a} EOSIO accounts, this may take significant minutes", ("a", accounts.size()) );
      for ( const auto &account : accounts )
      {
         try
         {
            abi_def abi;
            if ( abi_serializer::to_abi(account.abi, abi) )
            {
               if ( any_of(abi.tables.begin(), abi.tables.end(), [](const auto &v) { return v.name == N(accounts); }) )
               {
                  ilog( "have filtered token account '${a}'", ("a", account.name) );
                  _token_accounts.insert( account.name );
               }
            }
         }
         catch (...)
         {
            elog( "encountered an error while serializing abi of account '${a}'", ("a", account.name) );
         }
      }
      ilog( "scanning done! have totally filtered ${n} token accounts", ("n", _token_accounts.size()) );
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
      unordered_set<account_name> addons, recent_addons;
      for_each( tx.actions.begin(), tx.actions.end(), [&](const auto &a)
      {
         if ( valid_token_contract(_chain_plugin.get_read_only_api(), a) )
         {
            if ( _token_accounts.count(a.account) <= 0 )        addons.insert( a.account );
            if ( _recent_token_accounts.count(a.account) <= 0 ) recent_addons.insert( a.account );
         }
      });

      if ( addons.size() || recent_addons.size() )
      {
         unique_lock<shared_mutex> wl( _smutex );
         if ( addons.size() )
         {
            _token_accounts.insert( addons.begin(), addons.end() );
            ilog( "filtered ${n} new token accounts from transaction ${id}", ("n", addons.size())("id", tx_meta->id()) );
         }
         if ( recent_addons.size() )
         {
            _recent_token_accounts.insert( recent_addons.begin(), recent_addons.end() );
            ilog( "filtered ${n} new RECENT token accounts from transaction ${id}", ("n", recent_addons.size())("id", tx_meta->id()) );
         }
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
      vector<future<vector<io_params::get_account_tokens_result::code_assets>>> promises;
      auto params = parse_body<io_params::get_account_tokens_params>( body );
      shared_lock<shared_mutex> rl( _smutex );
      vector<account_name> codes( params.recent ? _recent_token_accounts.begin() : _token_accounts.begin(),
         params.recent ? _recent_token_accounts.end() : _token_accounts.end() );
      rl.unlock();
      ilog( "scanning ${t} tokens from account '${a}'", ("t", codes.size())("a", params.account_name) );
      for ( auto i = 0; i < _thread_num; ++i )
      {
         auto step = codes.size() / _thread_num;
         auto begin = i * step;
         auto end = (i + 1 < _thread_num) ? (i + 1) * step : codes.size();
         promises.emplace_back( async_thread_pool( _thread_pool.get_executor(), [this, &codes, account = params.account_name, begin, end]()
         {
            chain_apis::read_only::get_currency_balance_params cb_params {
               .account = account
            };
            vector<io_params::get_account_tokens_result::code_assets> tokens;
            auto read_only = _chain_plugin.get_read_only_api();
            for ( auto i = begin; i < end; ++i )
            {
               cb_params.code = codes[i];
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
                  unique_lock<shared_mutex> wl( _smutex );
                  _token_accounts.erase( cb_params.code );
               }
            }
            return tokens;
         }));
      }

      io_params::get_account_tokens_result account_tokens;
      for ( auto &promise : promises )
      {
         auto tokens = promise.get();
         account_tokens.tokens.insert( account_tokens.tokens.end(), tokens.begin(), tokens.end() );
      }

      fc::variant result;
      fc::to_variant( account_tokens, result );
      cb( 200, result );
   }
};

void query_api_plugin::set_program_options( options_description &, options_description &cfg )
{
   cfg.add_options()
      ("thread-pool-size", bpo::value<uint16_t>()->default_value(2), "number of threads in thread_pool.");
}

void query_api_plugin::plugin_initialize( const variables_map &options )
{
   ilog( "starting query_api_plugin" );

   auto pool_size = options.at("thread-pool-size").as<uint16_t>();
   EOS_ASSERT( pool_size > 0, plugin_config_exception, "invalid thread_pool size config (> 0)" );

   my.reset( new query_api_plugin_impl(app().get_plugin<chain_plugin>(), pool_size) );
   my->initialize();
}

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

FC_REFLECT( eosio::io_params::get_account_tokens_params, (account_name)(recent) )
FC_REFLECT( eosio::io_params::get_account_tokens_result, (tokens) )
FC_REFLECT( eosio::io_params::get_account_tokens_result::code_assets, (code)(assets) )
