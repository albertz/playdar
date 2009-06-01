#include "moost/http/filesystem_request_handler.hpp"
#include <fstream>
#include <sstream>
#include <string>
#include <boost/lexical_cast.hpp>

#include "moost/http/mime_types.hpp"
#include "moost/http/reply.hpp"
#include "moost/http/request.hpp"

using namespace moost::http;

void filesystem_request_handler::handle_request(const request& req, reply& rep)
{
  // Decode url to path.
  std::string request_path;
  if (!url_decode(req.uri, request_path))
  {
    rep.stock_reply(reply::bad_request);
    return;
  }

  // Request path must be absolute and not contain "..".
  if (request_path.empty() || request_path[0] != '/'
      || request_path.find("..") != std::string::npos)
  {
    rep.stock_reply(reply::bad_request);
    return;
  }

  // If path ends in slash (i.e. is a directory) then add "index.html".
  if (request_path[request_path.size() - 1] == '/')
  {
    request_path += "index.html";
  }

  // Determine the file extension.
  std::size_t last_slash_pos = request_path.find_last_of("/");
  std::size_t last_dot_pos = request_path.find_last_of(".");
  std::string extension;
  if (last_dot_pos != std::string::npos && last_dot_pos > last_slash_pos)
  {
    extension = request_path.substr(last_dot_pos + 1);
  }

  // Open the file to send back.
  std::string full_path = doc_root_ + request_path;
  std::ifstream is(full_path.c_str(), std::ios::in | std::ios::binary);
  if (!is)
  {
    rep.stock_reply(reply::not_found);
    return;
  }

  // Fill out the reply to be sent to the client.
  rep.set_status( reply::ok );
  std::string content;
  char buf[512];
  while (is.read(buf, sizeof(buf)).gcount() > 0)
    content.append(buf, is.gcount());
   
  rep.add_header("Content-Length", content.size());
  rep.add_header("Content-Type", mime_types::extension_to_type(extension));
  rep.write_content(content);

  //rep.headers.resize(2);
  //rep.headers[0].name = "Content-Length";
  //rep.headers[0].value = boost::lexical_cast<std::string>(rep.content.size());
  //rep.headers[1].name = "Content-Type";
  //rep.headers[1].value = mime_types::extension_to_type(extension);
}

bool filesystem_request_handler::url_decode(const std::string& in, std::string& out)
{
  out.clear();
  out.reserve(in.size());
  for (std::size_t i = 0; i < in.size(); ++i)
  {
    if (in[i] == '%')
    {
      if (i + 3 <= in.size())
      {
        int value;
        std::istringstream is(in.substr(i + 1, 2));
        if (is >> std::hex >> value)
        {
          out += static_cast<char>(value);
          i += 2;
        }
        else
        {
          return false;
        }
      }
      else
      {
        return false;
      }
    }
    else if (in[i] == '+')
    {
      out += ' ';
    }
    else
    {
      out += in[i];
    }
  }
  return true;
}
