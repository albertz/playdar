/*
    Playdar - music content resolver
    Copyright (C) 2009  Richard Jones
    Copyright (C) 2009  Last.fm Ltd.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#include "lan.h"

#include "playdar/resolver_query.hpp"
#include "playdar/playdar_request.h"
#include "playdar/utils/htmlentities.hpp"
#include "playdar/logger.h"

#include <ctime>

/// port used for binding the udp endpoints only (nothing to do with tcp/http):
#define DEFAULT_LAN_PORT 60210
#define DEFAULT_LAN_ENDPOINT "239.255.0.1"

using namespace std;
using namespace json_spirit;

using namespace json_spirit;

namespace playdar {

using namespace utils;

namespace resolvers {


bool
lan::init(pa_ptr pap)
{
    m_pap = pap;
    setup_endpoints();
    if( m_endpoints.size() == 0 )
    {
        log::error() << "LAN Error, no valid endpoints configured" << endl;
        return false;
    }

    m_responder_thread.reset( new boost::thread(boost::bind(&lan::run, this)) );
    return true;
}

lan::~lan() throw()
{
    //cout << "DTOR LAN " << endl;
    //send_pang(); // can't send when shutting down - crashes atm.
    if( m_io_service )
        m_io_service->stop();
    
    if( m_responder_thread )
        m_responder_thread->join();
    
    if( socket_ )
        delete(socket_);
    
    //if( broadcast_endpoint_ )
    //    delete(broadcast_endpoint_);
}

/// decide what udp endpoints queries will be sent to.
/// if nothing is specified, just use the default multicast
/// otherwise send to everything in the list from the config file
void
lan::setup_endpoints()
{
    Value endpoints = m_pap->get_json("endpoints");
    if( endpoints == Value::null || endpoints.type() != array_type )
    {
        // nothing specified, default is just the multicast address:
        m_endpoints.push_back( 
            new boost::asio::ip::udp::endpoint
            (  boost::asio::ip::address::from_string
               (DEFAULT_LAN_ENDPOINT), DEFAULT_LAN_PORT )
        );
        log::info() << "LAN plugin using default multicast address of " << DEFAULT_LAN_ENDPOINT << endl;
    }
    else // manual config of endpoints:
    {
        vector<Value> eps = endpoints.get_array();
        BOOST_FOREACH( Value v, eps )
        {
            unsigned short port = DEFAULT_LAN_PORT;
            string ip;
            if( v.type() == str_type )
            {
                ip = v.get_str();
            } 
            else if( v.type() != array_type )
            {
                continue;
            }
            else
            {
                vector<Value> pairv = v.get_array();
                if( pairv.size() == 0 ) continue;
                if( pairv[0].type() != str_type ) continue;
                // set port, if specified in the array:
                if( pairv.size() > 1 && pairv[1].type() == int_type )
                    port = pairv[1].get_int();
                ip = pairv[0].get_str();
            }
            // add new endpoint to the vector:    
            try
            {
                m_endpoints.push_back( 
                 new boost::asio::ip::udp::endpoint(
                 boost::asio::ip::address::from_string( ip ), port ) );    
            }catch(...){}
        }
        log::info() << "LAN plugin has " << m_endpoints.size() << " endpoints configured." << endl;
    }
}

void
lan::start_resolving(boost::shared_ptr<ResolverQuery> rq)
{
    using namespace json_spirit;
    ostringstream querystr;
    write_formatted( rq->get_json(), querystr );
    //log::info() << "Resolving: " << querystr.str() << " through the LAN plugin" << endl;
    async_send(querystr.str());
}

void 
lan::cancel_query(query_uid qid)
{
    /*
       Our options include:
        1) broadcast the cancel to all, so everyone can clean up
        2) send cancel to people we initially sent it to (would mean keeping track, meh)
        3) do nothing, let their reaper jobs clean up after a timeout. 
           at least we cleaned up ourselves, but remote nodes will just have to timeout->cleanup.
       
       Going with (3) for now, until I see proof that memory consumption by stale queries is 
       causing a significant problem.
    */ 
}

void 
lan::run()
{
    m_io_service.reset( new boost::asio::io_service );
    // it's very rare that you'd need to manually specify the listen port+ip:
    start_listening(*m_io_service,
                    boost::asio::ip::address::from_string("0.0.0.0"),
                    boost::asio::ip::address::from_string
                    ( m_pap->get<string>("listenip", DEFAULT_LAN_ENDPOINT) ),
                    m_pap->get<int>("listenport", DEFAULT_LAN_PORT) ); 
    
    log::info() << "LAN Resolver is online udp://" 
         << socket_->local_endpoint().address() << ":"
         << socket_->local_endpoint().port()
         << endl;
    send_ping(); // announce our presence to the LAN
    m_io_service->run();
}

void 
lan::start_listening(boost::asio::io_service& io_service,
        const boost::asio::ip::address& listen_address,
        const boost::asio::ip::address& multicast_address,
        const short multicast_port)
{ 
    socket_ = new boost::asio::ip::udp::socket(io_service);
    // Create the socket so that multiple may be bound to the same address.
    boost::asio::ip::udp::endpoint listen_endpoint(
            listen_address, multicast_port);
    socket_->open(listen_endpoint.protocol());
    socket_->set_option(boost::asio::ip::udp::socket::reuse_address(true));
    socket_->bind(listen_endpoint);

    // Join the multicast group.
    socket_->set_option(
            boost::asio::ip::multicast::join_group(multicast_address));

    socket_->async_receive_from(
            boost::asio::buffer(data_, max_length), sender_endpoint_,
            boost::bind(&lan::handle_receive_from, this,
                boost::asio::placeholders::error,
                boost::asio::placeholders::bytes_transferred));
                
}

/// send to all configured endpoints:
void 
lan::async_send( const string& message )
{
    BOOST_FOREACH( boost::asio::ip::udp::endpoint * ep, m_endpoints )
    {
        async_send( ep, message ); 
    }
}

/// send to specific endpoints:
void 
lan::async_send(boost::asio::ip::udp::endpoint * remote_endpoint,
                const string& message)
{
    if( message.length() > max_length )
    {
        log::error() << "WARNING outgoing UDP message is rather large, haven't tested this, discarding." << endl;
        return;
    }

    // you can set numcopies to 2 or 3 for lossy networks:
    int copies = m_pap->get<int>("numcopies", 1);
    if(copies<1) copies=1;
    for(int j = 0; j<copies; j++)
    {
        char * buf = (char*)malloc(message.length());
        memcpy(buf, message.data(), message.length());
        socket_->async_send_to(     
                boost::asio::buffer(buf,message.length()), 
                *remote_endpoint,
                boost::bind(&lan::handle_send, this,
                    boost::asio::placeholders::error,
                    boost::asio::placeholders::bytes_transferred,
                    buf));
    }
}

void lan::handle_send(   const boost::system::error_code& error,
                                size_t bytes_recvd,
                                char * scratch )
{
    // free the memory that was holding the message we just sent.
    if(scratch)
    {
        free(scratch);
    }
}


void 
lan::handle_receive_from(const boost::system::error_code& error,
        size_t bytes_recvd)
{
    if (!error)
    {
        std::ostringstream msgs;
        msgs.write(data_, bytes_recvd);
        std::string msg = msgs.str();

        boost::asio::ip::address sender_address = sender_endpoint_.address();

        do
        {
            
            if( sender_address.to_string() == "127.0.0.1" )
            {   
                // TODO detect our actual LAN IP and bail out here
                // if it came from our IP.
                // Will bail anyway once parsed and dupe QID noticed,
                // but more efficient to do it here.
                // cout << "* Ignoring udp msg from self" << endl;
                break;
            }
            
            //cout    << "lan: Received multicast message (from " 
            //        << sender_address.to_string() << "):" 
            //        << endl << msg << endl;
            
            using namespace json_spirit;
            // try and parse it as json:
            Value mv;
            if(!read(msg, mv)) 
            {
                log::warning() << "lan: invalid JSON in this message, discarding." << endl;
                break; // Invalid JSON, ignore it.
            }
            Object qo = mv.get_obj();
            map<string,Value> r;
            obj_to_map(qo,r);
            
            // we identify JSON messages by the "_msgtype" property:
            string msgtype = "";
            if( r.find("_msgtype")!=r.end() &&
                r["_msgtype"].type() == str_type )
            {
                msgtype = r["_msgtype"].get_str();
            }
            else
            {
                log::warning() << "UDP msg rcvd without _msgtype - discarding"
                     << endl;
                break;
            }
            
            if(msgtype == "rq") // REQUEST / NEW QUERY
            {
                boost::shared_ptr<ResolverQuery> rq;
                try
                {
                    rq = ResolverQuery::from_json(qo);
                } 
                catch (...) 
                {
                    log::warning() << "lan: missing fields in JSON query object, discarding" << endl;
                    break; 
                }
                
                if(m_pap->query_exists(rq->id()))
                {
                    //cout << "lan: discarding message, QID already exists: " << rq->id() << endl;
                    break;
                }
                
                // dispatch query with our callback that will
                // respond to the searcher via UDP.
                rq_callback_t cb =
                 boost::bind(&lan::send_response, this, _1, _2,
                             sender_endpoint_);
                query_uid qid = m_pap->dispatch(rq, cb);
            }
            else if(msgtype == "result") // RESPONSE 
            {
                Object resobj = r["result"].get_obj();
                map<string,Value> resobj_map;
                obj_to_map(resobj, resobj_map);
                query_uid qid = r["qid"].get_str();
                if(!m_pap->query_exists(qid))
                {
                    log::warning() << "lan: Ignoring response - QID invalid or expired" << endl;
                    break;
                }
                //cout << "lan: Got udp response." <<endl;

                vector< Object > final_results;
                try
                {
                    ResolvedItem ri(resobj);

                    if (ri.id().length()) {
                        ostringstream rbs;
                        rbs << "http://"
                        << sender_endpoint_.address()
                        << ":"
                        << sender_endpoint_.port()
                        << "/sid/"
                        << ri.id();
                        ri.set_url( rbs.str() );
                    }

                    final_results.push_back( ri.get_json() );
                    m_pap->report_results( qid, final_results );
                    //cout    << "INFO Result from '" << rip->source()
                    //        <<"' for '"<< write_formatted( rip->get_json())
                    //        << endl;
                }
                catch (...)
                {
                    log::warning() << "lan: Missing fields in response json, discarding" << endl;
                    break;
                }
            }
            else if(msgtype == "ping")
            {
                receive_ping( r, sender_endpoint_ );
            }
            else if(msgtype == "pong")
            {
                receive_pong( r, sender_endpoint_ );
            }
            else if(msgtype == "pang")
            {
                receive_pang( r, sender_endpoint_ );
            }
            
        }while(false);
                
        socket_->async_receive_from(
                boost::asio::buffer(data_, max_length), sender_endpoint_,
                boost::bind(&lan::handle_receive_from, this,
                    boost::asio::placeholders::error,
                    boost::asio::placeholders::bytes_transferred));
    }
    else
    {
        log::warning() << "Some error for udp" << endl;
    }
}

// fired when a new result is available for a running query:
void
lan::send_response( query_uid qid, 
                        ri_ptr rip,
                        boost::asio::ip::udp::endpoint sep )
{
    //cout << "lan responding for " << qid << " to: " 
    //     << sep.address().to_string() 
    //     << " score: " << rip->score()
    //     << endl;
    using namespace json_spirit;
    Object response;
    response.reserve(3);
    response.push_back( Pair("_msgtype", "result") );
    response.push_back( Pair("qid", qid) );
    
    // strip the url from _a copy_ of the result_item
    ResolvedItem tmp( *rip );
    tmp.rm_json_value( "url" );
    response.push_back( Pair("result", tmp.get_json()) );

    async_send( &sep, write_formatted( response ) );
}

// LAN presence stuff.

/// broadcast ping to LAN and see who's out there
void
lan::send_ping()
{
    log::info() << "LAN sending ping.." << endl;
    using namespace json_spirit;
    Object jq;
    jq.push_back( Pair("_msgtype", "ping") );
    jq.push_back( Pair("from_name", m_pap->hostname()) );
    jq.push_back( Pair("http_port", m_pap->get("http_port", 8888)) ); //TODO get from config?
    ostringstream os;
    write_formatted( jq, os );
    async_send(os.str());
}

/// pong reply back to specific user
void
lan::send_pong(boost::asio::ip::udp::endpoint sender_endpoint)
{
    log::info() << "LAN sending pong back to " 
         << sender_endpoint.address().to_string() <<".." << endl;
    Object o;
    o.push_back( Pair("_msgtype", "pong") );
    o.push_back( Pair("from_name", m_pap->hostname()) );
    o.push_back( Pair("http_port", m_pap->get("http_port", 8888)) );//TODO get from config?
    ostringstream os;
    write_formatted( o, os );
    async_send( &sender_endpoint, os.str() );
}

/// called when we shutdown - uses a blocking send due to shutdown mechanics.
void
lan::send_pang()
{
    log::info() << "LAN sending pang.." << endl;
    using namespace json_spirit;
    Object o;
    o.push_back( Pair("_msgtype", "pang") );
    o.push_back( Pair("from_name", m_pap->hostname()) );
    ostringstream os;
    write_formatted( o, os );
    async_send(os.str());
}

void
lan::receive_ping(map<string,Value> & om,
                      const boost::asio::ip::udp::endpoint &  sender_endpoint)
{
    if(om.find("from_name")==om.end() || om["from_name"].type()!=str_type)
    {
        log::warning() << "Malformed UDP PING dropped." << endl;
        return;
    }
    // ignore pings sent from ourselves:
    if(om["from_name"]==m_pap->hostname()) return;
    if(om.find("http_port")==om.end() || om["http_port"].type()!=int_type)
    {
        log::warning() << "Malformed UDP PING dropped." << endl;
        return;
    }
    string from_name = om["from_name"].get_str();
    log::info() << "Received UDP PING from '" << from_name 
         << "' @ " << sender_endpoint.address().to_string()
         << endl;
    ostringstream hbase;
    hbase   << "http://" << sender_endpoint.address().to_string() 
            << ":" << om["http_port"].get_int();
    lannode node;
    time(&node.lastdate);
    node.name = from_name;
    node.http_base = hbase.str();
    node.udp_ep = sender_endpoint;
    m_lannodes[from_name] = node;
    send_pong( sender_endpoint );
}

void
lan::receive_pong(map<string,Value> & om,
                      const boost::asio::ip::udp::endpoint &  sender_endpoint)
{
    if(om.find("from_name")==om.end() || om["from_name"].type()!=str_type)
    {
        log::warning() << "Malformed UDP PONG dropped." << endl;
        return;
    }
    if(om.find("http_port")==om.end() || om["http_port"].type()!=int_type)
    {
        log::warning() << "Malformed UDP PONG dropped." << endl;
        return;
    }
    string from_name = om["from_name"].get_str();
    log::info() << "Received UDP PONG from '" << from_name 
         << "' @ " << sender_endpoint.address().to_string()
         << endl;
    ostringstream hbase;
    hbase   << "http://" << sender_endpoint.address().to_string() 
            << ":" << om["http_port"].get_int();
    
    lannode node;
    time(&node.lastdate);
    node.name = from_name;
    node.http_base = hbase.str();
    node.udp_ep = sender_endpoint;
    m_lannodes[from_name] = node;
}

void
lan::receive_pang(map<string,Value> & om,
                  const boost::asio::ip::udp::endpoint &  sender_endpoint)
{
    if(om.find("from_name")==om.end() || om["from_name"].type()!=str_type)
    {
        log::warning() << "Malformed UDP PANG dropped." << endl;
        return;
    }
    string from_name = om["from_name"].get_str();
    log::info() << "Received UDP PANG from '" << from_name 
         << "' @ " << sender_endpoint.address().to_string()
         << endl;
    m_lannodes.erase(from_name);
}

bool endsWith(const std::string& s, const std::string& tail)
{
    const std::string::size_type slen = s.length(), tlen = tail.length();
    return slen >= tlen ? (s.substr(slen - tlen) == tail) : false;
}

bool
lan::anon_http_handler(const playdar_request& req, playdar_response& resp, playdar::auth& /*pauth*/)
{
    log::info() << "request handler on lan for url: " << req.url() << endl;

    time_t now;
    time(&now);
    typedef std::pair<string, lannode> LanPair;

    if (endsWith(req.url(), "roster")) { 
        Array a;
        BOOST_FOREACH(const LanPair& p, m_lannodes)
        {
            Object o;
            o.push_back( Pair("name", p.first) );
            o.push_back( Pair("address", p.second.http_base) );
            //FIXME not safe on compilers where sizeof (long) > sizeof(int) after the year 2038
            o.push_back( Pair("age", (int)(now - p.second.lastdate)) );
            a.push_back(o);
        }
        ostringstream os;
        write_formatted(a, os);
        resp = playdar_response(os.str(), false);
        return true;
    }

    ostringstream os;
    os  << "<h2>LAN</h2>"
        "<p>Detected nodes:"
        "<table>" 
        "<tr style=\"font-weight:bold;\">"
        "<td>Name</td> <td>Address</td> <td>Seconds since last ping</td>"
        "</td>" << endl;
    BOOST_FOREACH( const LanPair& p, m_lannodes )
    {
        os  << "<tr><td>" << htmlentities(p.first) << "</td>"
            "<td><a href=\"" << htmlentities(p.second.http_base) << "/\">"<< htmlentities(p.second.http_base) <<"</a></td>"
            "<td>" << (now - p.second.lastdate) << "</td>"
            "</tr>" << endl;
    }
    os  << "</ul></p>" << endl;
    
    resp = os.str();
    return true;
}


}}
