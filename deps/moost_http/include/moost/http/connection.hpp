#ifndef __MOOST_HTTP_CONNECTION_HPP__
#define __MOOST_HTTP_CONNECTION_HPP__

#include <boost/bind.hpp>
#include <boost/asio.hpp>
#include <boost/array.hpp>
#include <boost/noncopyable.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/enable_shared_from_this.hpp>

#include "moost/http/reply.hpp"
#include "moost/http/request.hpp"
#include "moost/http/request_handler_base.hpp"
#include "moost/http/request_parser.hpp"

namespace moost { namespace http {

/// Represents a single connection from a client.
template<class RequestHandler>
class connection
  : public boost::enable_shared_from_this< connection<RequestHandler> >,
    private boost::noncopyable
{
public:
  /// Construct a connection with the given io_service.
  explicit connection(boost::asio::io_service& io_service,
      request_handler_base<RequestHandler>& handler);

  /// Get the socket associated with the connection.
  boost::asio::ip::tcp::socket& socket();

  /// Start the first asynchronous operation for the connection.
  void start();

private:
  /// Handle completion of a read operation.
  void handle_read(const boost::system::error_code& e,
      std::size_t bytes_transferred);

  /// Handle completion of a handle_read and handle_write operations:
  void handle_write(const boost::system::error_code& e);

  void do_async_write(boost::asio::const_buffer buffer);

  /// Strand to ensure the connection's handlers are not called concurrently.
  boost::asio::io_service::strand strand_;

  /// Socket for the connection.
  boost::asio::ip::tcp::socket socket_;

  /// The handler used to process the incoming request.
  request_handler_base<RequestHandler>& request_handler_;

  /// Buffer for incoming data.
  static const int buffer_size_ = 8192; //TODO make configurable?
  char buffer_[buffer_size_];
  //boost::array<char, 8192> buffer_;

  /// The incoming request.
  request request_;

  /// The parser for the incoming request.
  request_parser request_parser_;

  /// The reply to be sent back to the client.
  reply reply_;
};

template<class RequestHandler>
connection<RequestHandler>::connection(boost::asio::io_service& io_service,
    request_handler_base<RequestHandler>& handler)
: strand_(io_service),
  socket_(io_service),
  request_handler_(handler)
{
}

template<class RequestHandler>
void connection<RequestHandler>::start()
{
  socket_.async_read_some(boost::asio::buffer(buffer_, buffer_size_),
      strand_.wrap(
        boost::bind(&connection<RequestHandler>::handle_read, connection<RequestHandler>::shared_from_this(),
          boost::asio::placeholders::error,
          boost::asio::placeholders::bytes_transferred)));
}

template<class RequestHandler>
void connection<RequestHandler>::handle_read( const boost::system::error_code& e,
                                              std::size_t bytes_transferred )
{
  if (!e)
  {
    boost::tribool result;
    boost::tie(result, boost::tuples::ignore) = request_parser_.parse(
        request_, buffer_, buffer_ + bytes_transferred);
   
    if ( boost::indeterminate(result) )
    {
      // need to read more
      socket_.async_read_some(boost::asio::buffer(buffer_, buffer_size_),
          strand_.wrap(
            boost::bind(&connection<RequestHandler>::handle_read, this->shared_from_this(),
              boost::asio::placeholders::error,
              boost::asio::placeholders::bytes_transferred)));
      return; // we're all done here!
    }

    if ( result )
       request_handler_.handle_request_base(request_, reply_);
    else if ( !result )
       reply_ = reply::stock_reply(reply::bad_request);

    boost::asio::async_write(socket_, reply_.to_buffers_headers(),
         strand_.wrap(
           boost::bind(&connection<RequestHandler>::handle_write, this->shared_from_this(),
             boost::asio::placeholders::error)));
  }

  // If an error occurs then no new asynchronous operations are started. This
  // means that all shared_ptr references to the connection object will
  // disappear and the object will be destroyed automatically after this
  // handler returns. The connection class's destructor closes the socket.
}

template<class RequestHandler>
boost::asio::ip::tcp::socket& connection<RequestHandler>::socket()
{
  return socket_;
}

template<class RequestHandler>
void connection<RequestHandler>::handle_write(const boost::system::error_code& e)
{
    if (reply_.get_async_delegate()) {
        if (e) {
            // error, signal to the delegate we're done
            reply_.get_async_delegate()(0);
            return;
        }

        // content_async_write can return false to end the write
        if (reply_.get_async_delegate()(   
            boost::bind(
            &connection<RequestHandler>::do_async_write,
            this->shared_from_this(),
            _1)))
        {
            return; // good
        }
    } else if (e) {
        return;
    }

    // all done here!
    // Initiate graceful connection closure.
    boost::system::error_code ignored_ec;
    socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored_ec);
}

// this is the write function we pass to the content_async_write callback
// write a zero-length buffer to close the socket and end the callback chain
//
template<class RequestHandler>
void connection<RequestHandler>::do_async_write(boost::asio::const_buffer buffer)
{
    if (boost::asio::detail::buffer_size_helper(buffer) > 0) {
        boost::asio::async_write(socket_, boost::asio::const_buffers_1(buffer), 
            strand_.wrap(
                boost::bind(&connection<RequestHandler>::handle_write, this->shared_from_this(),
                    boost::asio::placeholders::error)));
    } else {
        // all done here!
        // Initiate graceful connection closure.
        boost::system::error_code ignored_ec;
        socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored_ec);
    }
}

}} // moost::http

#endif // __MOOST_HTTP_CONNECTION_HPP__
