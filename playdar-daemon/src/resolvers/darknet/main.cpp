/*
    Test harness used to bootstrap servent.
*/
#include <iostream>
#include <boost/asio.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/thread.hpp>
#include <iostream>
#include <string>
#include <boost/algorithm/string.hpp>

#include "msgs.h"
#include "protocol.h"
#include "servent.h"

using namespace playdar::darknet;

void start_io(boost::asio::io_service *io_service)
{
	io_service->run();
    cout << "Main io_service exiting!" << endl;   
}


int main(int argc, char* argv[])
{
    try
    {
        // Check command line arguments.
        if (argc != 3)
        {
        std::cerr << "Usage: servent <port> <username>" << std::endl;
        return 1;
        }
        unsigned short port = boost::lexical_cast<unsigned short>(argv[1]);
        string username(argv[2]);
        boost::asio::io_service io_service;
        boost::asio::io_service io_service_p;
        boost::shared_ptr<playdar::darknet::Protocol> 
                protocol(new Protocol(username, io_service_p));
        Servent servent(io_service, port, protocol);
        boost::thread thr(boost::bind(start_io, &io_service));
        boost::thread thrp(boost::bind(start_io, &io_service_p));
        std::string line;
        while(1)
        {
            std::getline(std::cin, line);
            if(line=="quit") break;
            std::vector<std::string> toks;
            boost::split(toks, line, boost::is_any_of(" "));
            // establish new connection:
            if(toks[0]=="connect")
            {
                boost::asio::ip::address_v4 ip =
                    boost::asio::ip::address_v4::from_string(toks[1]);
                unsigned short port = boost::lexical_cast<unsigned short>(toks[2]);
                boost::asio::ip::tcp::endpoint ep(ip, port);
                servent.connect_to_remote(ep);
            }
            // send searchquery to all connections:
            if(toks[0]=="search")
            {
                msg_ptr msg(new LameMsg(toks[1], SEARCHQUERY));
                protocol->start_search(msg);
            }
        
        }
        cout << "exiting";
        io_service.stop();
        thr.join();
    }
    catch (std::exception& e)
    {
        std::cerr << e.what() << std::endl;
    }
    
    return 0;
}

