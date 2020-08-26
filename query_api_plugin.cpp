
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
   const controller &_ctrl;
   const chain_plugin &_chain_plugin;
   shared_mutex _smutex;
   unordered_set<account_name> _token_accounts;
   named_thread_pool _thread_pool;
   fc::optional<boost::signals2::scoped_connection> _accepted_transaction_connection;

public:
   static auto register_apis( const query_api_plugin_impl &impl )
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
   query_api_plugin_impl( const chain_plugin &chain, uint8_t thread_num )
      : _ctrl( chain.chain() )
      , _chain_plugin( chain )
      , _thread_pool( "query", static_cast<size_t>(thread_num) )
   {}

   void startup()
   {
      const auto &blog = _ctrl.block_log();
      auto first_block_num = blog.first_block_num();
      auto head_block_num = blog.head()->block_num();
      ilog( "scanning token contracts from block ${b} to block ${e} in block_log, this may take a few minutes.", ("b", first_block_num)("e", head_block_num) );
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
                  if ( a.name == N(transfer) )
                  {
                     _token_accounts.insert( a.account );
                  }
               });
            }
         });
      }

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
         if ( a.name == N(transfer) && _token_accounts.count(a.account) <= 0 )
         {
            addons.insert( a.account );
         }
      });

      if (! addons.empty() )
      {
         unique_lock<shared_mutex> wl( _smutex );
         _token_accounts.insert( addons.begin(), addons.end() );
      }
   }

   //=========================
   // HTTP API implements
   //=========================

   void get_token_contracts( url_response_callback &&cb ) const
   {
      fc::variant result;
      shared_lock<shared_mutex> rl( _smutex );
      fc::to_variant( _token_accounts, result );
      rl.unlock();
      cb( 200, result );
   }

   void get_account_tokens( string &&body, url_response_callback &&cb ) const
   {
      io_params::get_account_tokens_result account_tokens;
      async_thread_pool( _thread_pool.get_executor(), [&]()
      {
         auto params = parse_body<io_params::get_account_tokens_params>( body );
         chain_apis::read_only::get_currency_balance_params cb_params {
            .account = params.account_name
         };
         auto read_only = _chain_plugin.get_read_only_api();
         shared_lock<shared_mutex> rl( _smutex );
         for ( const auto &code : _token_accounts )
         {
            cb_params.code = code;
            vector<asset> assets = read_only.get_currency_balance( cb_params );
            if (! assets.empty() )
            {
               account_tokens.tokens.emplace_back( io_params::get_account_tokens_result::code_assets {
                  .code   = code,
                  .assets = assets
               });
            }
         }
         rl.unlock();
      }).get();

      fc::variant result;
      fc::to_variant( account_tokens, result );
      cb( 200, result );
   }
};

query_api_plugin::query_api_plugin() {}
query_api_plugin::~query_api_plugin() {}

// API plugin no need to do these
void query_api_plugin::set_program_options(options_description&, options_description& cfg) {}
void query_api_plugin::plugin_initialize(const variables_map& options) {}

// set up API handler
void query_api_plugin::plugin_startup()
{
   ilog( "starting query_api_plugin" );

   auto &http = app().get_plugin<http_plugin>();
   auto &chain = app().get_plugin<chain_plugin>();
   my.reset( new query_api_plugin_impl(chain, 5) );
   http.add_api( query_api_plugin_impl::register_apis(*my) );
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
