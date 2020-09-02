
* 需要修改controller.hpp/cpp文件, 不然无法编译通过

hpp中加入:
#include <eosio/chain/block_log.hpp>
const block_log& block_log()const;

cpp中加入:
//#include <eosio/chain/block_log.hpp>
const block_log& controller::block_log()const { return my->blog; }

* 参数描述

在config.ini中使用的参数配置:
thread-pool-size (单次访问接口/v1/query/get_account_tokens提供的最大并发查找次数， 默认为2)
blocknum-scan-from (节点启动时插件从block_log中扫描区块的起始区块高度，默认为0)
blocknum-scan-to (节点启动时插件从block_log中扫描区块的结束区块高度，默认为-1)

在命令行中使用的参数配置:
accounts-json (用文件保存通过访问接口/v1/query/get_token_accounts返回的代币合约列表json数据，节点启动时指定此文件可以初始化插件的代币列表，通过设置blocknum-scan-from能够避免长时间的扫描操作)
