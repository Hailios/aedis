/* Copyright (c) 2018-2022 Marcelo Zimbres Silva (mzimbres@gmail.com)
 *
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE.txt)
 */

#include <boost/asio.hpp>
#if defined(BOOST_ASIO_HAS_CO_AWAIT)
#include <aedis.hpp>
#include "common/common.hpp"

namespace net = boost::asio;
namespace resp3 = aedis::resp3;
using aedis::adapt;

auto run(std::shared_ptr<connection> conn) -> net::awaitable<void>
{
   co_await connect(conn, "127.0.0.1", "6379");
   co_await conn->async_run();
}

// Called from the main function (see main.cpp)
auto async_main() -> net::awaitable<void>
{
   resp3::request req;
   req.push("HELLO", 3);
   req.push("PING", "Hello world");
   req.push("QUIT");

   std::tuple<aedis::ignore, std::string, aedis::ignore> resp;

   auto ex = co_await net::this_coro::executor;
   auto conn = std::make_shared<connection>(ex);
   net::co_spawn(ex, run(conn), net::detached);
   co_await conn->async_exec(req, adapt(resp));

   std::cout << "PING: " << std::get<1>(resp) << std::endl;
}

#endif // defined(BOOST_ASIO_HAS_CO_AWAIT)
