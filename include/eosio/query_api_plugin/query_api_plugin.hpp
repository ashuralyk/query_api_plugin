#pragma once
#include <appbase/application.hpp>

namespace eosio
{

using namespace appbase;
using namespace chain;

/**
 *  This is a template plugin, intended to serve as a starting point for making new plugins
 */
class query_api_plugin
   : public appbase::plugin<query_api_plugin>
{
public:
   query_api_plugin() {}
   virtual ~query_api_plugin() {}
 
   APPBASE_PLUGIN_REQUIRES()
   virtual void set_program_options( options_description &cli, options_description &cfg ) override;
 
   void plugin_initialize( const variables_map &options );
   void plugin_startup();
   void plugin_shutdown();

private:
   std::unique_ptr<class query_api_plugin_impl> my;
};

}
