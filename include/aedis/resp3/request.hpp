/* Copyright (c) 2018-2022 Marcelo Zimbres Silva (mzimbres@gmail.com)
 *
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE.txt)
 */

#ifndef AEDIS_RESP3_REQUEST_HPP
#define AEDIS_RESP3_REQUEST_HPP

#include <aedis/resp3/type.hpp>

#include <string>
#include <tuple>
#include <memory_resource>

// NOTE: Consider detecting tuples in the type in the parameter pack
// to calculate the header size correctly.
//
// NOTE: For some commands like hset it would be a good idea to assert
// the value type is a pair.

namespace aedis::resp3 {

constexpr char const* separator = "\r\n";

/** @brief Adds a bulk to the request.
 *  @relates request
 *
 *  This function is useful in serialization of your own data
 *  structures in a request. For example
 *
 *  @code
 *  void to_bulk(std::string& to, mystruct const& obj)
 *  {
 *     auto const str = // Convert obj to a string.
 *     resp3::to_bulk(to, str);
 *  }
 *  @endcode
 *
 *  @param to Storage on which data will be copied into.
 *  @param data Data that will be serialized and stored in @c to.
 *
 *  See more in @ref serialization.
 */
template <class Request>
void to_bulk(Request& to, std::string_view data)
{
   auto const str = std::to_string(data.size());

   to += to_code(type::blob_string);
   to.append(std::cbegin(str), std::cend(str));
   to += separator;
   to.append(std::cbegin(data), std::cend(data));
   to += separator;
}

template <class Request, class T, typename = typename std::enable_if<std::is_integral<T>::value>::type>
void to_bulk(Request& to, T n)
{
   auto const s = std::to_string(n);
   to_bulk(to, std::string_view{s});
}

namespace detail {

auto has_response(std::string_view cmd) -> bool;

template <class T>
struct add_bulk_impl {
   template <class Request>
   static void add(Request& to, T const& from)
   {
      using namespace aedis::resp3;
      to_bulk(to, from);
   }
};

template <class ...Ts>
struct add_bulk_impl<std::tuple<Ts...>> {
   template <class Request>
   static void add(Request& to, std::tuple<Ts...> const& t)
   {
      auto f = [&](auto const&... vs)
      {
         using namespace aedis::resp3;
         (to_bulk(to, vs), ...);
      };

      std::apply(f, t);
   }
};

template <class U, class V>
struct add_bulk_impl<std::pair<U, V>> {
   template <class Request>
   static void add(Request& to, std::pair<U, V> const& from)
   {
      using namespace aedis::resp3;
      to_bulk(to, from.first);
      to_bulk(to, from.second);
   }
};

template <class Request>
void add_header(Request& to, type t, std::size_t size)
{
   auto const str = std::to_string(size);

   to += to_code(t);
   to.append(std::cbegin(str), std::cend(str));
   to += separator;
}

template <class Request, class T>
void add_bulk(Request& to, T const& data)
{
   detail::add_bulk_impl<T>::add(to, data);
}

template <class>
struct bulk_counter;

template <class>
struct bulk_counter {
  static constexpr auto size = 1U;
};

template <class T, class U>
struct bulk_counter<std::pair<T, U>> {
  static constexpr auto size = 2U;
};

template <class Request>
void add_blob(Request& to, std::string_view blob)
{
   to.append(std::cbegin(blob), std::cend(blob));
   to += separator;
}

template <class Request>
void add_separator(Request& to)
{
   to += separator;
}
} // detail

/** \brief Creates Redis requests.
 *  \ingroup high-level-api
 *  
 *  A request is composed of one or more Redis commands and is
 *  referred to in the redis documentation as a pipeline, see
 *  https://redis.io/topics/pipelining. For example
 *
 *  @code
 *  request r;
 *  r.push("HELLO", 3);
 *  r.push("FLUSHALL");
 *  r.push("PING");
 *  r.push("PING", "key");
 *  r.push("QUIT");
 *  @endcode
 *
 *  \remarks
 *
 *  \li Non-string types will be converted to string by using \c
 *  to_bulk, which must be made available over ADL.
 *  \li Uses a std::pmr::string for internal storage.
 */
class request {
public:
   /// Request configuration options.
   struct config {
      /** \brief If `true` 
       * `aedis::connection::async_exec` will complete with error if the
       * connection is lost. Affects only requests that haven't been
       * sent yet.
       */
      bool cancel_on_connection_lost = true;

      /** \brief If `true` the request will be coalesced with other
       * requests, see https://redis.io/topics/pipelining. Otherwise
       * the request is sent individually.
       */
      bool coalesce = true;

      /** \brief If `true` the request will complete with
       * aedis::error::not_connected if `async_exec` is called before
       * the connection with Redis was established.
       */
      bool cancel_if_not_connected = false;

      /** \brief If `false` `aedis::connection::async_exec` will not
       * automatically cancel this request if the connection is lost.
       * Affects only requests that have been written to the socket
       * but remained unresponded when `aedis::connection::async_run`
       * completed.
       */
      bool cancel_if_unresponded = true;

      /** \brief If this request has a `HELLO` command and this flag is
       * `true`, the `aedis::connection` will move it to the front of
       * the queue of awaiting requests. This makes it possible to
       * send `HELLO` and authenticate before other commands are sent.
       */
      bool hello_with_priority = true;
   };

   /** \brief Constructor
    *  
    *  \param cfg Configuration options.
    *  \param resource Memory resource.
    */
    explicit
    request(config cfg = config{true, true, false, true, true},
            std::pmr::memory_resource* resource = std::pmr::get_default_resource())
    : cfg_{cfg}, payload_(resource) {}

    //// Returns the number of commands contained in this request.
   [[nodiscard]] auto size() const noexcept -> std::size_t
      { return commands_;};

   [[nodiscard]] auto payload() const noexcept -> auto const&
      { return payload_;}

   [[nodiscard]] auto has_hello_priority() const noexcept -> auto const&
      { return has_hello_priority_;}

   /// Clears the request preserving allocated memory.
   void clear()
   {
      payload_.clear();
      commands_ = 0;
   }

   /// Calls std::pmr::string::reserve on the internal storage.
   void reserve(std::size_t new_cap = 0)
      { payload_.reserve(new_cap); }

   /// Returns a const reference to the config object.
   [[nodiscard]] auto get_config() const noexcept -> auto const& {return cfg_; }

   /// Returns a reference to the config object.
   [[nodiscard]] auto get_config() noexcept -> auto& {return cfg_; }

   /** @brief Appends a new command to the end of the request.
    *
    *  For example
    *
    *  \code
    *  request req;
    *  req.push("SET", "key", "some string", "EX", "2");
    *  \endcode
    *
    *  will add the \c set command with value "some string" and an
    *  expiration of 2 seconds.
    *
    *  \param cmd The command e.g redis or sentinel command.
    *  \param args Command arguments.
    */
   template <class... Ts>
   void push(std::string_view cmd, Ts const&... args)
   {
      using resp3::type;

      auto constexpr pack_size = sizeof...(Ts);
      detail::add_header(payload_, type::array, 1 + pack_size);
      detail::add_bulk(payload_, cmd);
      detail::add_bulk(payload_, std::tie(std::forward<Ts const&>(args)...));

      check_cmd(cmd);
   }

   /** @brief Appends a new command to the end of the request.
    *  
    *  This overload is useful for commands that have a key and have a
    *  dynamic range of arguments. For example
    *
    *  @code
    *  std::map<std::string, std::string> map
    *     { {"key1", "value1"}
    *     , {"key2", "value2"}
    *     , {"key3", "value3"}
    *     };
    *
    *  request req;
    *  req.push_range("HSET", "key", std::cbegin(map), std::cend(map));
    *  @endcode
    *  
    *  \param cmd The command e.g. Redis or Sentinel command.
    *  \param key The command key.
    *  \param begin Iterator to the begin of the range.
    *  \param end Iterator to the end of the range.
    */
   template <class Key, class ForwardIterator>
   void push_range(std::string_view cmd, Key const& key, ForwardIterator begin, ForwardIterator end,
                    typename std::iterator_traits<ForwardIterator>::value_type * = nullptr)
   {
      using value_type = typename std::iterator_traits<ForwardIterator>::value_type;
      using resp3::type;

      if (begin == end)
         return;

      auto constexpr size = detail::bulk_counter<value_type>::size;
      auto const distance = std::distance(begin, end);
      detail::add_header(payload_, type::array, 2 + size * distance);
      detail::add_bulk(payload_, cmd);
      detail::add_bulk(payload_, key);

      for (; begin != end; ++begin)
	 detail::add_bulk(payload_, *begin);

      check_cmd(cmd);
   }

   /** @brief Appends a new command to the end of the request.
    *
    *  This overload is useful for commands that have a dynamic number
    *  of arguments and don't have a key. For example
    *
    *  \code
    *  std::set<std::string> channels
    *     { "channel1" , "channel2" , "channel3" }
    *
    *  request req;
    *  req.push("SUBSCRIBE", std::cbegin(channels), std::cend(channels));
    *  \endcode
    *
    *  \param cmd The Redis command
    *  \param begin Iterator to the begin of the range.
    *  \param end Iterator to the end of the range.
    */
   template <class ForwardIterator>
   void push_range(std::string_view cmd, ForwardIterator begin, ForwardIterator end,
                   typename std::iterator_traits<ForwardIterator>::value_type * = nullptr)
   {
      using value_type = typename std::iterator_traits<ForwardIterator>::value_type;
      using resp3::type;

      if (begin == end)
         return;

      auto constexpr size = detail::bulk_counter<value_type>::size;
      auto const distance = std::distance(begin, end);
      detail::add_header(payload_, type::array, 1 + size * distance);
      detail::add_bulk(payload_, cmd);

      for (; begin != end; ++begin)
	 detail::add_bulk(payload_, *begin);

      check_cmd(cmd);
   }

   /** @brief Appends a new command to the end of the request.
    *  
    *  Equivalent to the overload taking a range (i.e. send_range2).
    *
    *  \param cmd Redis command.
    *  \param key Redis key.
    *  \param range Range to send e.g. and \c std::map.
    */
   template <class Key, class Range>
   void push_range(std::string_view cmd, Key const& key, Range const& range,
                   decltype(std::begin(range)) * = nullptr)
   {
      using std::begin;
      using std::end;
      push_range(cmd, key, begin(range), end(range));
   }

   /** @brief Appends a new command to the end of the request.
    *
    *  Equivalent to the overload taking a range (i.e. send_range2).
    *
    *  \param cmd Redis command.
    *  \param range Range to send e.g. and \c std::map.
    */
   template <class Range>
   void push_range(std::string_view cmd, Range const& range,
                   decltype(std::begin(range)) * = nullptr)
   {
      using std::begin;
      using std::end;
      push_range(cmd, begin(range), end(range));
   }

private:
   void check_cmd(std::string_view cmd)
   {
      if (!detail::has_response(cmd))
         ++commands_;

      if (cmd == "HELLO")
         has_hello_priority_ = cfg_.hello_with_priority;
   }

   config cfg_;
   std::pmr::string payload_;
   std::size_t commands_ = 0;
   bool has_hello_priority_ = false;
};

} // aedis::resp3

#endif // AEDIS_RESP3_SERIALIZER_HPP
