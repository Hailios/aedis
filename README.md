# Aedis

Aedis is a [Redis](https://redis.io/) client library built on top of
[Boost.Asio](https://www.boost.org/doc/libs/release/doc/html/boost_asio.html)
that implements the latest version of the Redis communication
protocol
[RESP3](https://github.com/redis/redis-specifications/blob/master/protocol/RESP3.md).
It makes communication with a Redis server easy by hiding low-level
code away from the user, which, in the majority of the cases will be
concerned with only three library entities

* `aedis::connection`: A connection to the Redis server with
  high-level functions to execute Redis commands, receive server
  pushes and support for automatic command
  [pipelines](https://redis.io/docs/manual/pipelining/).
* `aedis::resp3::request`: A container of Redis commands that supports
  STL containers and user defined data types.
* `aedis::adapt()`: A function that adapts data structures to receive responses.

In the next sections we will cover all those points in detail with
examples. The requirements for using Aedis are

* Boost 1.80 or greater.
* C++17 minimum.
* Redis 6 or higher (must support RESP3).
* Gcc (10, 11, 12), Clang (11, 13, 14) and Visual Studio (16 2019, 17 2022).
* Have basic-level knowledge about Redis and understand Asio and its asynchronous model.

Readers that are not familiar with Redis can learn more about
it on https://redis.io/docs/, in essence

> Redis is an open source (BSD licensed), in-memory data structure
> store used as a database, cache, message broker, and streaming
> engine. Redis provides data structures such as strings, hashes,
> lists, sets, sorted sets with range queries, bitmaps, hyperloglogs,
> geospatial indexes, and streams. Redis has built-in replication, Lua
> scripting, LRU eviction, transactions, and different levels of
> on-disk persistence, and provides high availability via Redis
> Sentinel and automatic partitioning with Redis Cluster.

<a name="connection"></a>
## Connection

Let us start with a simple application that uses a short-lived
connection to read Redis
[hashes](https://redis.io/docs/data-types/hashes/) in a `std::map`

```cpp
auto co_main() -> net::awaitable<void>
{
   auto conn = std::make_shared<connection>(co_await net::this_coro::executor);

   // From examples/common.hpp to avoid vebosity
   co_await connect(conn, "127.0.0.1", "6379");

   // A request can contain multiple commands.
   resp3::request req;
   req.push("HELLO", 3);
   req.push("HGETALL", "hset-key");
   req.push("QUIT");

   // The tuple elements will store the responses to each individual
   // command. The responses to HELLO and QUIT are being ignored for
   // simplicity.
   std::tuple<ignore, std::map<std::string, std::string>, ignore> resp;

   // Executes the request. See below why we are using operator ||.
   co_await (conn->async_run() || conn->async_exec(req, adapt(resp)));
   // Use the map from std::get<1>(resp) ...
}
```

The example above uses the Asio awaitable `operator ||` to compose
`connection::async_exec` and `connection::async_run` in an
operation we can `co_await` on. It also provides cancelation of one of
the operations when the other completes.  The role played by these
functions are

* `connection::async_exec`: Execute commands by queuing the request
  for writing and wait for the response sent back by
  Redis. Can be called from multiple places in your code concurrently.
* `connection::async_run`: Coordinate low-level read and write
  operations. More specifically, it will hand IO control to
  `async_exec` when a response arrives and to `async_receive` when a
  server-push is received. It is also responsible for triggering writes of pending
  requests.

The example above is also available in other programming styles for comparison

* cpp20_intro_awaitable_ops.cpp: The version shown above.
* cpp20_intro.cpp: Does not use awaitable operators.
* cpp20_intro_tls.cpp: Communicates over TLS.
* cpp17_intro.cpp: Uses callbacks and requires C++17.
* cpp17_intro_sync.cpp: Runs `async_run` in a separate thread and
  performs synchronous calls to `async_exec`.

For performance reasons we will usually want to perform multiple
requests with the same connection. We can do this in the example above
by letting `async_run` run detached in a separate coroutine, for
example (see cpp20_intro.cpp)

```cpp
auto run(std::shared_ptr<connection> conn) -> net::awaitable<void>
{
   co_await connect(conn, "127.0.0.1", "6379");
   co_await conn->async_run();
}

auto hello(std::shared_ptr<connection> conn) -> net::awaitable<void>
{
   resp3::request req;
   req.push("HELLO", 3);

   co_await conn->async_exec(req);
}

auto ping(std::shared_ptr<connection> conn) -> net::awaitable<void>
{
   resp3::request req;
   req.push("PING", "Hello world");

   std::tuple<std::string> resp;
   co_await conn->async_exec(req, adapt(resp));
   // Use the response ...
}

auto quit(std::shared_ptr<connection> conn) -> net::awaitable<void>
{
   resp3::request req;
   req.push("QUIT");

   co_await conn->async_exec(req);
}

auto co_main() -> net::awaitable<void>
{
   auto ex = co_await net::this_coro::executor;
   auto conn = std::make_shared<connection>(ex);
   net::co_spawn(ex, run(conn), net::detached);
   co_await hello(conn);
   co_await ping(conn);
   co_await quit(conn);

   // conn can be passed around to other coroutines that need to
   // communicate with Redis.  For example, sessions in a HTTP and
   // Websocket server.
}
```

With this separation, it is now easy to incorporate other long-running
operations in our application, for example, the run coroutine below
adds signal handling and a healthy checker (see cpp20_echo_server.cpp)

```cpp
auto run(std::shared_ptr<connection> conn) -> net::awaitable<void>
{
   signal_set sig{co_await net::this_coro::executor, SIGINT, SIGTERM};
   co_await connect(conn, "127.0.0.1", "6379");
   co_await (conn->async_run() || sig.async_wait() || healthy_checker(conn));
}
```

The definition of the `healthy_checker` used above can be found in common.cpp.

### Server pushes

Redis servers can also send a variety of pushes to the client, some of
them are

* [Pubsub](https://redis.io/docs/manual/pubsub/)
* [Keyspace notification](https://redis.io/docs/manual/keyspace-notifications/)
* [Client-side caching](https://redis.io/docs/manual/client-side-caching/)

The connection class supports server pushes by means of the
`aedis::connection::async_receive` function, the coroutine shows how
to used it

```cpp
auto receiver(std::shared_ptr<connection> conn) -> net::awaitable<void>
{
   using resp_type = std::vector<resp3::node<std::string>>;
   for (resp_type resp;;) {
      co_await conn->async_receive(adapt(resp));
      // Use resp and clear the response for a new push.
      resp.clear();
   }
}
```

The `receiver` defined above can be run detached or incorporated
in the `run` function as we did for the `healthy_checker`

```cpp
auto run(std::shared_ptr<connection> conn) -> net::awaitable<void>
{
   signal_set sig{co_await net::this_coro::executor, SIGINT, SIGTERM};
   co_await connect(conn, "127.0.0.1", "6379");
   co_await (conn->async_run() || sig.async_wait() || healthy_checker(conn) || receiver(conn));
}
```

### Reconnecting

Adding a loop around `async_run` produces a simple way to support
reconnection _while there are pending operations on the connection_,
for example, to reconnect to the same address (see cpp20_subscriber.cpp)

```cpp
auto run(std::shared_ptr<connection> conn) -> net::awaitable<void>
{
   steady_timer timer{co_await net::this_coro::executor};

   for (;;) {
      co_await connect(conn, "127.0.0.1", "6379");
      co_await (conn->async_run() || healthy_checker(conn) || receiver(conn));

      // Prepare the stream for a new connection.
      conn->reset_stream();

      // Waits one second before trying to reconnect.
      timer.expires_after(std::chrono::seconds{1});
      co_await timer.async_wait();
   }
}
```

This feature results in considerable simplification of backend code
and makes it easier to write failover-safe applications. For example,
a Websocket server might have a 10k sessions communicating with Redis at
the time the connection is lost (or maybe killed by the server admin
to force a failover). It would be concerning if each individual section
were to throw exceptions and handle error.  With the pattern shown
above the only place that has to manage the error is the run function.

### Cancellation

Aedis supports both implicit and explicit cancellation of connection
operations. Explicit cancellation is supported by means of the
`aedis::connection::cancel` member function. Implicit
terminal-cancellation, like those that happen when using Asio
awaitable `operator ||` will be discussed with more detail below.

```cpp
co_await (conn.async_run(...) || conn.async_exec(...))
```

* Useful for short-lived connections that are meant to be closed after
  a command has been executed.

```cpp
co_await (conn.async_exec(...) || time.async_wait(...))
```

* Provides a way to limit how long the execution of a single request
  should last.
* WARNING: If the timer fires after the request has been sent but before the
  response has been received, the connection will be closed.
* It is usually a better idea to have a healthy checker than adding
  per request timeout, see cpp20_subscriber.cpp for an example.

```cpp
co_await (conn.async_exec(...) || conn.async_exec(...) || ... || conn.async_exec(...))
```

* This works but is unnecessary. Unless the user has set
  `aedis::resp3::request::config::coalesce` to `false`, and he
  usually shouldn't, the connection will automatically merge the
  individual requests into a single payload.

<a name="requests"></a>
## Requests

Redis requests are composed of one or more commands (in the
Redis documentation they are called
[pipelines](https://redis.io/topics/pipelining)). For example

```cpp
// Some example containers.
std::list<std::string> list {...};
std::map<std::string, mystruct> map { ...};

// The request can contains multiple commands.
request req;

// Command with variable length of arguments.
req.push("SET", "key", "some value", "EX", "2");

// Pushes a list.
req.push_range("SUBSCRIBE", list);

// Same as above but as an iterator range.
req.push_range("SUBSCRIBE", std::cbegin(list), std::cend(list));

// Pushes a map.
req.push_range("HSET", "key", map);
```

Sending a request to Redis is performed with `aedis::connection::async_exec` as already stated.

<a name="serialization"></a>

### Serialization

The `resp3::request::push` and `resp3::request::push_range` member functions work
with integer data types e.g. `int` and `std::string` out of the box.
To send your own data type define a `to_bulk` function like this

```cpp
// User defined type.
struct mystruct {...};

// Serialize it in to_bulk.
void to_bulk(std::pmr::string& to, mystruct const& obj)
{
   std::string dummy = "Dummy serializaiton string.";
   aedis::resp3::to_bulk(to, dummy);
}
```

Once `to_bulk` is defined and visible over ADL `mystruct` can
be passed to the `request`

```cpp
request req;

std::map<std::string, mystruct> map {...};

req.push_range("HSET", "key", map);
```

Example cpp20_serialization.cpp shows how store json strings in Redis.

<a name="responses"></a>

### Config flags

The `aedis::resp3::request::config` object inside the request dictates how the
`aedis::connection` should handle the request in some important situations. The
reader is advised to read it carefully.

## Responses

Aedis uses the following strategy to support Redis responses

* **Static**: For `aedis::resp3::request` whose sizes are known at compile time
  `std::tuple` is supported.
* **Dynamic**: Otherwise use `std::vector<aedis::resp3::node<std::string>>`.

For example, below is a request with a compile time size

```cpp
request req;
req.push("PING");
req.push("INCR", "key");
req.push("QUIT");
```

To read the response to this request users can use the following tuple

```cpp
std::tuple<std::string, int, std::string>
```

The pattern might have become apparent to the reader: the tuple must
have as many elements as the request has commands (exceptions below).
It is also necessary that each tuple element is capable of storing the
response to the command it refers to, otherwise an error will occur.
To ignore responses to individual commands in the request use the tag
`aedis::ignore`

```cpp
// Ignore the second and last responses.
std::tuple<std::string, aedis::ignore, std::string, aedis::ignore>
```

The following table provides the resp3-types returned by some Redis
commands

Command  | RESP3 type                          | Documentation
---------|-------------------------------------|--------------
lpush    | Number                              | https://redis.io/commands/lpush
lrange   | Array                               | https://redis.io/commands/lrange
set      | Simple-string, null or blob-string  | https://redis.io/commands/set
get      | Blob-string                         | https://redis.io/commands/get
smembers | Set                                 | https://redis.io/commands/smembers
hgetall  | Map                                 | https://redis.io/commands/hgetall

To map these RESP3 types into a C++ data structure use the table below

RESP3 type     | Possible C++ type                                            | Type
---------------|--------------------------------------------------------------|------------------
Simple-string  | `std::string`                                              | Simple
Simple-error   | `std::string`                                              | Simple
Blob-string    | `std::string`, `std::vector`                               | Simple
Blob-error     | `std::string`, `std::vector`                               | Simple
Number         | `long long`, `int`, `std::size_t`, `std::string`           | Simple
Double         | `double`, `std::string`                                    | Simple
Null           | `std::optional<T>`                                         | Simple
Array          | `std::vector`, `std::list`, `std::array`, `std::deque`     | Aggregate
Map            | `std::vector`, `std::map`, `std::unordered_map`            | Aggregate
Set            | `std::vector`, `std::set`, `std::unordered_set`            | Aggregate
Push           | `std::vector`, `std::map`, `std::unordered_map`            | Aggregate

For example, the response to the request

```cpp
request req;
req.push("HELLO", 3);
req.push_range("RPUSH", "key1", vec);
req.push_range("HSET", "key2", map);
req.push("LRANGE", "key3", 0, -1);
req.push("HGETALL", "key4");
req.push("QUIT");

```

can be read in the tuple below

```cpp
std::tuple<
   aedis::ignore,  // hello
   int,            // rpush
   int,            // hset
   std::vector<T>, // lrange
   std::map<U, V>, // hgetall
   std::string     // quit
> resp;
```

Where both are passed to `async_exec` as showed elsewhere

```cpp
co_await conn->async_exec(req, adapt(resp));
```

If the intention is to ignore the response to all commands altogether
use `adapt()` without arguments instead

```cpp
// Uses the ignore adapter explicitly.
co_await conn->async_exec(req, adapt());

// Ignore adapter is also the default argument.
co_await conn->async_exec(req);
```

Responses that contain nested aggregates or heterogeneous data
types will be given special treatment later in [The general case](#the-general-case).  As
of this writing, not all RESP3 types are used by the Redis server,
which means in practice users will be concerned with a reduced
subset of the RESP3 specification.

### Pushes

Commands that have no response like

* `"SUBSCRIBE"`
* `"PSUBSCRIBE"`
* `"UNSUBSCRIBE"`

must be **NOT** be included in the response tuple. For example, the request below

```cpp
request req;
req.push("PING");
req.push("SUBSCRIBE", "channel");
req.push("QUIT");
```

must be read in this tuple `std::tuple<std::string, std::string>`,
that has size two.

### Null

It is not uncommon for apps to access keys that do not exist or
that have already expired in the Redis server, to deal with these
cases Aedis provides support for `std::optional`. To use it,
wrap your type around `std::optional` like this

```cpp
std::tuple<
   std::optional<A>,
   std::optional<B>,
   ...
   > resp;

co_await conn->async_exec(req, adapt(resp));
```

Everything else stays pretty much the same.

### Transactions

To read responses to transactions we must first observe that Redis will
queue the transaction commands and send their individual responses as elements
of an array, the array is itself the response to the `EXEC` command.
For example, to read the response to this request

```cpp
req.push("MULTI");
req.push("GET", "key1");
req.push("LRANGE", "key2", 0, -1);
req.push("HGETALL", "key3");
req.push("EXEC");
```

use the following response type

```cpp
using aedis::ignore;

using exec_resp_type = 
   std::tuple<
      std::optional<std::string>, // get
      std::optional<std::vector<std::string>>, // lrange
      std::optional<std::map<std::string, std::string>> // hgetall
   >;

std::tuple<
   aedis::ignore,  // multi
   aedis::ignore,  // get
   aedis::ignore,  // lrange
   aedis::ignore,  // hgetall
   exec_resp_type, // exec
> resp;

co_await conn->async_exec(req, adapt(resp));
```

For a complete example see cpp20_containers.cpp.

### Deserialization

As mentioned in the serialization section, it is common practice to
serialize data before sending it to Redis e.g. as json strings.  For
performance and convenience reasons, we may also want to deserialize
responses directly in their final data structure. Aedis supports this
use case by calling a user provided `from_bulk` function while parsing
the response. For example

```cpp
void from_bulk(mystruct& obj, char const* p, std::size_t size, boost::system::error_code& ec)
{
   // Deserializes p into obj.
}
```

After that, you can start receiving data efficiently in the desired
types e.g. `mystruct`, `std::map<std::string, mystruct>` etc.

<a name="the-general-case"></a>

### The general case

There are cases where responses to Redis
commands won't fit in the model presented above, some examples are

* Commands (like `set`) whose responses don't have a fixed
RESP3 type. Expecting an `int` and receiving a blob-string
will result in error.
* RESP3 aggregates that contain nested aggregates can't be read in STL containers.
* Transactions with a dynamic number of commands can't be read in a `std::tuple`.

To deal with these cases Aedis provides the `aedis::resp3::node` type
abstraction, that is the most general form of an element in a
response, be it a simple RESP3 type or the element of an aggregate. It
is defined like this

```cpp
template <class String>
struct node {
   // The RESP3 type of the data in this node.
   type data_type;

   // The number of elements of an aggregate (or 1 for simple data).
   std::size_t aggregate_size;

   // The depth of this node in the response tree.
   std::size_t depth;

   // The actual data. For aggregate types this is always empty.
   String value;
};
```

Any response to a Redis command can be received in a
`std::vector<node<std::string>>`.  The vector can be seen as a
pre-order view of the response tree.  Using it is not different than
using other types

```cpp
// Receives any RESP3 simple or aggregate data type.
std::vector<node<std::string>> resp;
co_await conn->async_exec(req, adapt(resp));
```

For example, suppose we want to retrieve a hash data structure
from Redis with `HGETALL`, some of the options are

* `std::vector<node<std::string>`: Works always.
* `std::vector<std::string>`: Efficient and flat, all elements as string.
* `std::map<std::string, std::string>`: Efficient if you need the data as a `std::map`.
* `std::map<U, V>`: Efficient if you are storing serialized data. Avoids temporaries and requires `from_bulk` for `U` and `V`.

In addition to the above users can also use unordered versions of the
containers. The same reasoning applies to sets e.g. `SMEMBERS`
and other data structures in general.

## Examples

The examples below show how to use the features discussed so far

* cpp20_intro_awaitable_ops.cpp: The version shown above.
* cpp20_intro.cpp: Does not use awaitable operators.
* cpp20_intro_tls.cpp: Communicates over TLS.
* cpp20_containers.cpp: Shows how to send and receive STL containers and how to use transactions.
* cpp20_serialization.cpp: Shows how to serialize types using Boost.Json.
* cpp20_resolve_with_sentinel.cpp: Shows how to resolve a master address using sentinels.
* cpp20_subscriber.cpp: Shows how to implement pubsub with reconnection re-subscription.
* cpp20_echo_server.cpp: A simple TCP echo server.
* cpp20_chat_room.cpp: A command line chat built on Redis pubsub.
* cpp20_low_level_async.cpp: Sends a ping asynchronously using the low-level API.
* cpp17_low_level_sync.cpp: Sends a ping synchronously using the low-level API.
* cpp17_intro.cpp: Uses callbacks and requires C++17.
* cpp17_intro_sync.cpp: Runs `async_run` in a separate thread and performs synchronous calls to `async_exec`.

To avoid repetition code that is common to some examples has been
grouped in common.hpp. The main function used in some async examples
has been factored out in the main.cpp file.

## Echo server benchmark

This document benchmarks the performance of TCP echo servers I
implemented in different languages using different Redis clients.  The
main motivations for choosing an echo server are

   * Simple to implement and does not require expertise level in most languages.
   * I/O bound: Echo servers have very low CPU consumption in general
     and  therefore are excelent to  measure how a program handles concurrent requests.
   * It simulates very well a typical backend in regard to concurrency.

I also imposed some constraints on the implementations

   * It should be simple enough and not require writing too much code.
   * Favor the use standard idioms and avoid optimizations that require expert level.
   * Avoid the use of complex things like connection and thread pool.

To reproduce these results run one of the echo-server programs in one
terminal and the
[echo-server-client](https://github.com/mzimbres/aedis/blob/42880e788bec6020dd018194075a211ad9f339e8/benchmarks/cpp/asio/echo_server_client.cpp)
in another.

### Without Redis

First I tested a pure TCP echo server, i.e. one that sends the messages
directly to the client without interacting with Redis. The result can
be seen below

![](https://mzimbres.github.io/aedis/tcp-echo-direct.png)

The tests were performed with a 1000 concurrent TCP connections on the
localhost where latency is 0.07ms on average on my machine. On higher
latency networks the difference among libraries is expected to
decrease. 

   * I expected Libuv to have similar performance to Asio and Tokio.
   * I did expect nodejs to come a little behind given it is is
     javascript code. Otherwise I did expect it to have similar
     performance to libuv since it is the framework behind it.
   * Go did surprise me: faster than nodejs and libuv!

The code used in the benchmarks can be found at

   * [Asio](https://github.com/mzimbres/aedis/blob/3fb018ccc6138d310ac8b73540391cdd8f2fdad6/benchmarks/cpp/asio/echo_server_direct.cpp): A variation of [this](https://github.com/chriskohlhoff/asio/blob/4915cfd8a1653c157a1480162ae5601318553eb8/asio/src/examples/cpp20/coroutines/echo_server.cpp) Asio example.
   * [Libuv](https://github.com/mzimbres/aedis/tree/835a1decf477b09317f391eddd0727213cdbe12b/benchmarks/c/libuv): Taken from [here](https://github.com/libuv/libuv/blob/06948c6ee502862524f233af4e2c3e4ca876f5f6/docs/code/tcp-echo-server/main.c) Libuv example .
   * [Tokio](https://github.com/mzimbres/aedis/tree/3fb018ccc6138d310ac8b73540391cdd8f2fdad6/benchmarks/rust/echo_server_direct): Taken from [here](https://docs.rs/tokio/latest/tokio/).
   * [Nodejs](https://github.com/mzimbres/aedis/tree/3fb018ccc6138d310ac8b73540391cdd8f2fdad6/benchmarks/nodejs/echo_server_direct)
   * [Go](https://github.com/mzimbres/aedis/blob/3fb018ccc6138d310ac8b73540391cdd8f2fdad6/benchmarks/go/echo_server_direct.go)

### With Redis

This is similar to the echo server described above but messages are
echoed by Redis and not by the echo-server itself, which acts
as a proxy between the client and the Redis server. The results
can be seen below

![](https://mzimbres.github.io/aedis/tcp-echo-over-redis.png)

The tests were performed on a network where latency is 35ms on
average, otherwise it uses the same number of TCP connections
as the previous example.

As the reader can see, the Libuv and the Rust test are not depicted
in the graph, the reasons are

   * [redis-rs](https://github.com/redis-rs/redis-rs): This client
     comes so far behind that it can't even be represented together
     with the other benchmarks without making them look insignificant.
     I don't know for sure why it is so slow, I suppose it has
     something to do with its lack of automatic
     [pipelining](https://redis.io/docs/manual/pipelining/) support.
     In fact, the more TCP connections I lauch the worse its
     performance gets.

   * Libuv: I left it out because it would require me writing to much
     c code. More specifically, I would have to use hiredis and
     implement support for pipelines manually.

The code used in the benchmarks can be found at

   * [Aedis](https://github.com/mzimbres/aedis): [code](https://github.com/mzimbres/aedis/blob/3fb018ccc6138d310ac8b73540391cdd8f2fdad6/examples/echo_server.cpp)
   * [node-redis](https://github.com/redis/node-redis): [code](https://github.com/mzimbres/aedis/tree/3fb018ccc6138d310ac8b73540391cdd8f2fdad6/benchmarks/nodejs/echo_server_over_redis)
   * [go-redis](https://github.com/go-redis/redis): [code](https://github.com/mzimbres/aedis/blob/3fb018ccc6138d310ac8b73540391cdd8f2fdad6/benchmarks/go/echo_server_over_redis.go)

### Conclusion

Redis clients have to support automatic pipelining to have competitive performance. For updates to this document follow https://github.com/mzimbres/aedis.

## Comparison

The main reason for why I started writing Aedis was to have a client
compatible with the Asio asynchronous model. As I made progresses I could
also address what I considered weaknesses in other libraries.  Due to
time constraints I won't be able to give a detailed comparison with
each client listed in the
[official](https://redis.io/docs/clients/#cpp) list,
instead I will focus on the most popular C++ client on github in number of
stars, namely

* https://github.com/sewenew/redis-plus-plus

### Aedis vs Redis-plus-plus

Before we start it is important to mention some of the things
redis-plus-plus does not support

* The latest version of the communication protocol RESP3. Without that it is impossible to support some important Redis features like client side caching, among other things.
* Coroutines.
* Reading responses directly in user data structures to avoid creating temporaries.
* Error handling with support for error-code.
* Cancellation.

The remaining points will be addressed individually.  Let us first
have a look at what sending a command a pipeline and a transaction
look like

```cpp
auto redis = Redis("tcp://127.0.0.1:6379");

// Send commands
redis.set("key", "val");
auto val = redis.get("key"); // val is of type OptionalString.
if (val)
    std::cout << *val << std::endl;

// Sending pipelines
auto pipe = redis.pipeline();
auto pipe_replies = pipe.set("key", "value")
                        .get("key")
                        .rename("key", "new-key")
                        .rpush("list", {"a", "b", "c"})
                        .lrange("list", 0, -1)
                        .exec();

// Parse reply with reply type and index.
auto set_cmd_result = pipe_replies.get<bool>(0);
// ...

// Sending a transaction
auto tx = redis.transaction();
auto tx_replies = tx.incr("num0")
                    .incr("num1")
                    .mget({"num0", "num1"})
                    .exec();

auto incr_result0 = tx_replies.get<long long>(0);
// ...
```

Some of the problems with this API are

* Heterogeneous treatment of commands, pipelines and transaction. This makes auto-pipelining impossible.
* Any Api that sends individual commands has a very restricted scope of usability and should be avoided for performance reasons.
* The API imposes exceptions on users, no error-code overload is provided.
* No way to reuse the buffer for new calls to e.g. redis.get in order to avoid further dynamic memory allocations.
* Error handling of resolve and connection not clear.

According to the documentation, pipelines in redis-plus-plus have
the following characteristics

> NOTE: By default, creating a Pipeline object is NOT cheap, since
> it creates a new connection.

This is clearly a downside in the API as pipelines should be the
default way of communicating and not an exception, paying such a
high price for each pipeline imposes a severe cost in performance.
Transactions also suffer from the very same problem.

> NOTE: Creating a Transaction object is NOT cheap, since it
> creates a new connection.

In Aedis there is no difference between sending one command, a
pipeline or a transaction because requests are decoupled
from the IO objects.

> redis-plus-plus also supports async interface, however, async
> support for Transaction and Subscriber is still on the way.
> 
> The async interface depends on third-party event library, and so
> far, only libuv is supported.

Async code in redis-plus-plus looks like the following

```cpp
auto async_redis = AsyncRedis(opts, pool_opts);

Future<string> ping_res = async_redis.ping();

cout << ping_res.get() << endl;
```
As the reader can see, the async interface is based on futures
which is also known to have a bad performance.  The biggest
problem however with this async design is that it makes it
impossible to write asynchronous programs correctly since it
starts an async operation on every command sent instead of
enqueueing a message and triggering a write when it can be sent.
It is also not clear how are pipelines realised with this design
(if at all).

<a name="api-reference"></a>
## Reference

* [High-Level](#high-level-api): Covers the topics discussed in this document.
* [Low-Level](#low-level-api): Covers low-level building blocks. Provided mostly for developers, users won't usually need any information provided here.

## Installation

Download the latest release on
https://github.com/mzimbres/aedis/releases.  Aedis is a header only
library, so you can starting using it right away by adding the
`include` subdirectory to your project and including

```cpp
#include <aedis/src.hpp>
```

in no more than one source file in your applications. To build the
examples and tests cmake is supported, for example

```cpp
BOOST_ROOT=/opt/boost_1_80_0 cmake --preset dev
```
## Acknowledgement

Acknowledgement to people that helped shape Aedis

* Richard Hodges ([madmongo1](https://github.com/madmongo1)): For very helpful support with Asio, the design of asynchronous programs, etc.
* Vinícius dos Santos Oliveira ([vinipsmaker](https://github.com/vinipsmaker)): For useful discussion about how Aedis consumes buffers in the read operation.
* Petr Dannhofer ([Eddie-cz](https://github.com/Eddie-cz)): For helping me understand how the `AUTH` and `HELLO` command can influence each other.
* Mohammad Nejati ([ashtum](https://github.com/ashtum)): For pointing out scenarios where calls to `async_exec` should fail when the connection is lost.
* Klemens Morgenstern ([klemens-morgenstern](https://github.com/klemens-morgenstern)): For useful discussion about timeouts, cancellation, synchronous interfaces and general help with Asio.
* Vinnie Falco ([vinniefalco](https://github.com/vinniefalco)): For general suggestions about how to improve the code and the documentation.

## Changelog

### v1.4.0-1

* Renames `retry_on_connection_lost` to `cancel_if_unresponded`.  (v1.4.1)
* Removes dependency on Boost.Hana, boost::string_view, Boost.Variant2 and Boost.Spirit.
* Fixes build and setup CI on windows.

### v1.3.0-1

* Upgrades to Boost 1.80.0

* Removes automatic sending of the `HELLO` command. This can't be
  implemented properly without bloating the connection class. It is
  now a user responsibility to send HELLO. Requests that contain it have
  priority over other requests and will be moved to the front of the
  queue, see `aedis::resp3::request::config` 

* Automatic name resolving and connecting have been removed from
  `aedis::connection::async_run`. Users have to do this step manually
  now. The reason for this change is that having them built-in doesn't
  offer enough flexibility that is need for boost users.

* Removes healthy checks and idle timeout. This functionality must now
  be implemented by users, see the examples. This is
  part of making Aedis useful to a larger audience and suitable for
  the Boost review process.

* The `aedis::connection` is now using a typeddef to a
  `net::ip::tcp::socket` and  `aedis::ssl::connection` to
  `net::ssl::stream<net::ip::tcp::socket>`.  Users that need to use
  other stream type must now specialize `aedis::basic_connection`.

* Adds a low level example of async code.

### v1.2.0

* `aedis::adapt` supports now tuples created with `std::tie`.
  `aedis::ignore` is now an alias to the type of `std::ignore`.

* Provides allocator support for the internal queue used in the
  `aedis::connection` class.

* Changes the behaviour of `async_run` to complete with success if
  asio::error::eof is received. This makes it easier to  write
  composed operations with awaitable operators.

* Adds allocator support in the `aedis::resp3::request` (a
  contribution from Klemens Morgenstern).

* Renames `aedis::resp3::request::push_range2` to `push_range`. The
  suffix 2 was used for disambiguation. Klemens fixed it with SFINAE.

* Renames `fail_on_connection_lost` to
  `aedis::resp3::request::config::cancel_on_connection_lost`. Now, it will
  only cause connections to be canceled when `async_run` completes.

* Introduces `aedis::resp3::request::config::cancel_if_not_connected` which will
  cause a request to be canceled if `async_exec` is called before a
  connection has been established.

* Introduces new request flag `aedis::resp3::request::config::retry` that if
  set to true will cause the request to not be canceled when it was
  sent to Redis but remained unresponded after `async_run` completed.
  It provides a way to avoid executing commands twice.

* Removes the `aedis::connection::async_run` overload that takes
  request and adapter as parameters.

* Changes the way `aedis::adapt()` behaves with
  `std::vector<aedis::resp3::node<T>>`. Receiving RESP3 simple errors,
  blob errors or null won't causes an error but will be treated as
  normal response.  It is the user responsibility to check the content
  in the vector.

* Fixes a bug in `connection::cancel(operation::exec)`. Now this
  call will only cancel non-written requests.

* Implements per-operation implicit cancellation support for
  `aedis::connection::async_exec`. The following call will `co_await (conn.async_exec(...) || timer.async_wait(...))`
  will cancel the request as long as it has not been written.

* Changes `aedis::connection::async_run` completion signature to
  `f(error_code)`. This is how is was in the past, the second
  parameter was not helpful.

* Renames `operation::receive_push` to `aedis::operation::receive`.

### v1.1.0-1

* Removes `coalesce_requests` from the `aedis::connection::config`, it
  became a request property now, see `aedis::resp3::request::config::coalesce`.

* Removes `max_read_size` from the `aedis::connection::config`. The maximum
  read size can be specified now as a parameter of the
  `aedis::adapt()` function.

* Removes `aedis::sync` class, see intro_sync.cpp for how to perform
  synchronous and thread safe calls. This is possible in Boost. 1.80
  only as it requires `boost::asio::deferred`. 

* Moves from `boost::optional` to `std::optional`. This is part of
  moving to C++17.

* Changes the behaviour of the second `aedis::connection::async_run` overload
  so that it always returns an error when the connection is lost.

* Adds TLS support, see intro_tls.cpp.

* Adds an example that shows how to resolve addresses over sentinels,
  see subscriber_sentinel.cpp.

* Adds a `aedis::connection::timeouts::resp3_handshake_timeout`. This is
  timeout used to send the `HELLO` command.

* Adds `aedis::endpoint` where in addition to host and port, users can
  optionally provide username, password and the expected server role
  (see `aedis::error::unexpected_server_role`).

* `aedis::connection::async_run` checks whether the server role received in
  the hello command is equal to the expected server role specified in
  `aedis::endpoint`. To skip this check let the role variable empty.

* Removes reconnect functionality from `aedis::connection`. It
  is possible in simple reconnection strategies but bloats the class
  in more complex scenarios, for example, with sentinel,
  authentication and TLS. This is trivial to implement in a separate
  coroutine. As a result the `enum event` and `async_receive_event`
  have been removed from the class too.

* Fixes a bug in `connection::async_receive_push` that prevented
  passing any response adapter other that `adapt(std::vector<node>)`.

* Changes the behaviour of `aedis::adapt()` that caused RESP3 errors
  to be ignored. One consequence of it is that `connection::async_run`
  would not exit with failure in servers that required authentication.

* Changes the behaviour of `connection::async_run` that would cause it
  to complete with success when an error in the
  `connection::async_exec` occurred.

* Ports the buildsystem from autotools to CMake.

### v1.0.0

* Adds experimental cmake support for windows users.

* Adds new class `aedis::sync` that wraps an `aedis::connection` in
  a thread-safe and synchronous API.  All free functions from the
  `sync.hpp` are now member functions of `aedis::sync`.

* Split `aedis::connection::async_receive_event` in two functions, one
  to receive events and another for server side pushes, see
  `aedis::connection::async_receive_push`.

* Removes collision between `aedis::adapter::adapt` and
  `aedis::adapt`.

* Adds `connection::operation` enum to replace `cancel_*` member
  functions with a single cancel function that gets the operations
  that should be cancelled as argument.

* Bugfix: a bug on reconnect from a state where the `connection` object
  had unsent commands. It could cause `async_exec` to never
  complete under certain conditions.

* Bugfix: Documentation of `adapt()` functions were missing from
  Doxygen.

### v0.3.0

* Adds `experimental::exec` and `receive_event` functions to offer a
  thread safe and synchronous way of executing requests across
  threads. See `intro_sync.cpp` and `subscriber_sync.cpp` for
  examples.

* `connection::async_read_push` was renamed to `async_receive_event`.

* `connection::async_receive_event` is now being used to communicate
  internal events to the user, such as resolve, connect, push etc. For
  examples see cpp20_subscriber.cpp and `connection::event`.

* The `aedis` directory has been moved to `include` to look more
  similar to Boost libraries. Users should now replace `-I/aedis-path`
  with `-I/aedis-path/include` in the compiler flags.

* The `AUTH` and `HELLO` commands are now sent automatically. This change was
  necessary to implement reconnection. The username and password
  used in `AUTH` should be provided by the user on
  `connection::config`.

* Adds support for reconnection. See `connection::enable_reconnect`.

* Fixes a bug in the `connection::async_run(host, port)` overload
  that was causing crashes on reconnection.

* Fixes the executor usage in the connection class. Before theses
  changes it was imposing `any_io_executor` on users.

* `connection::async_receiver_event` is not cancelled anymore when
  `connection::async_run` exits. This change makes user code simpler.

* `connection::async_exec` with host and port overload has been
  removed. Use the other `connection::async_run` overload.

* The host and port parameters from `connection::async_run` have been
  move to `connection::config` to better support authentication and
  failover.

* Many simplifications in the `chat_room` example.

* Fixes build in clang the compilers and makes some improvements in
  the documentation.

### v0.2.0-1

* Fixes a bug that happens on very high load. (v0.2.1) 
* Major rewrite of the high-level API. There is no more need to use the low-level API anymore.
* No more callbacks: Sending requests follows the ASIO asynchronous model.
* Support for reconnection: Pending requests are not canceled when a connection is lost and are re-sent when a new one is established.
* The library is not sending HELLO-3 on user behalf anymore. This is important to support AUTH properly.

### v0.1.0-2

* Adds reconnect coroutine in the `echo_server` example. (v0.1.2)
* Corrects `client::async_wait_for_data` with `make_parallel_group` to launch operation. (v0.1.2)
* Improvements in the documentation. (v0.1.2)
* Avoids dynamic memory allocation in the client class after reconnection. (v0.1.2)
* Improves the documentation and adds some features to the high-level client. (v.0.1.1)
* Improvements in the design and documentation.

### v0.0.1

* First release to collect design feedback.

