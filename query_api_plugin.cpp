#include <eosio/chain_plugin/chain_plugin.hpp>
#include <eosio/http_plugin/http_plugin.hpp>
#include <eosio/chain/controller.hpp>
#include <eosio/query_api_plugin/query_api_plugin.hpp>

namespace eosio 
{

static appbase::abstract_plugin& _query_api_plugin = app().register_plugin<query_api_plugin>();

using namespace eosio;
using namespace std;

class query_api_plugin_impl
{
   const controller &_ctrl;

public:
   query_api_plugin_impl( const controller &ctrl )
      : _ctrl( ctrl )
   {}

   static auto api_get_token_contracts( const query_api_plugin_impl &impl )
   {
      return api_description::value_type {
         "/v1/query/get_token_contracts",
         [&] (string, string, url_response_callback cb) { return impl.get_token_contracts(move(cb)); }
      };
   }

   void get_token_contracts( url_response_callback &&cb ) const
   {
      const auto &blog = _ctrl.block_log();
      auto first_block_num = blog.first_block_num();
      auto head_block_num = blog.head()->block_num();
      ilog( "scanning token contracts from block ${b} to block ${e}", ("b", first_block_num)("e", head_block_num) );
      unordered_set<account_name> token_accounts;
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
                     token_accounts.insert( a.account );
                  }
               });
            }
         });
      }
      fc::variant result;
      fc::to_variant( token_accounts, result );
      cb( 200, result );
   }
};

query_api_plugin::query_api_plugin(){}
query_api_plugin::~query_api_plugin(){}

// API plugin no need to do these
void query_api_plugin::set_program_options(options_description&, options_description& cfg) {}
void query_api_plugin::plugin_initialize(const variables_map& options) {}

// set up API handler
void query_api_plugin::plugin_startup() {
   ilog( "starting query_api_plugin" );

   const auto &ctrl = app().get_plugin<chain_plugin>().chain();
   my.reset( new query_api_plugin_impl(ctrl) );
   auto& _http_plugin = app().get_plugin<http_plugin>();
   _http_plugin.add_api({
      query_api_plugin_impl::api_get_token_contracts( *my )
   });
}

void query_api_plugin::plugin_shutdown() {
   // OK, that's enough magic
}

}
