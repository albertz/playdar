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
#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/foreach.hpp>
#include <boost/program_options.hpp>
#include <boost/thread.hpp>

#include <iostream>
#include <cstdio>
#include <fstream>
#include <iterator>

#include "playdar/application.h"
#include <curl/curl.h>

#include "playdar/playdar_request_handler.h"
#include "playdar/logger.h"

using namespace std;
using namespace playdar;
namespace po = boost::program_options;

// global because the sighandler needs it
MyApplication * app = 0;

static void sigfunc(int sig)
{
    log::info() << "Signal handler triggered" << endl;
    if ( app )
        app->shutdown(sig);
}

// A helper function to simplify the main part.
template<class T>
ostream& operator<<(ostream& os, const vector<T>& v)
{
    copy(v.begin(), v.end(), ostream_iterator<T>(cout, " "));
    return os;
}

/// finds config dir, checks default locations etc
string find_config_dir()
{
    using boost::filesystem::path;

#if __APPLE__
    if(getenv("HOME"))
    {
        path home = getenv("HOME");
        return (home/"Library/Preferences/playdar").string();
    }
    else
    {
        log::error() << "Error, $HOME not set." << endl;
        throw;
    }
#elif _WIN32
    return ""; //TODO refer to Qt documentation to get code to do this
#else
    string p;
    if(getenv("XDG_CONFIG_HOME"))
    {
        p = getenv("XDG_CONFIG_HOME");
    }
    else if(getenv("HOME"))
    {
        p = string(getenv("HOME")) + "/.config";
    }
    else
    {
        log::error() << "Error, $HOME or $XDG_CONFIG_HOME not set." << endl;
        throw;
    }
    path config_base = p;
    string confpath =  (config_base/"playdar").string();
    if( boost::filesystem::exists(confpath) ) 
        return confpath;
    if( boost::filesystem::exists("/etc/playdar") )
        return "/etc/playdar";
    // fail
    return "";
#endif
}

void start_http_server(string ip, int port, int conc, MyApplication* app)
{
    if(conc<1) conc=1;
    log::info() << "HTTP server starting on: http://" << ip << ":" << port << "/" << " with " << conc << " threads" << endl;
    moost::http::server<playdar_request_handler> s(ip, port, conc);
    s.request_handler().init(app);
    // tell app how to stop the http server:
    app->set_http_stopper( 
        boost::bind(&moost::http::server<playdar_request_handler>::stop, &s));
    try 
    {
        s.run();
    }
    catch( const boost::system::system_error& e )
    {
        log::error() << "HTTP server error: " << e.what() << endl;
    }
    log::info() << "http_server thread exiting." << endl; 
}

static void print_curl_info()
{
    // print some curl version info
    curl_version_info_data * cv = curl_version_info(CURLVERSION_NOW);
    if(cv->age >= 0)
    {
        log::info() << "Curl version:\t" << cv->version << endl;
        const char * proto;
        int i = 0;
        log::info() << "* Protocols:\t";
        for(; (proto = cv->protocols[i]) ; i++ )
        {
            log::info() << proto << ", " ;
        }
        log::info() 
            << endl
            << "* SSL:\t" << (cv->features&CURL_VERSION_SSL ? "YES" : "NO") << endl
            << "* IPv6:\t" << (cv->features&CURL_VERSION_IPV6 ? "YES" : "NO") << endl
            << "* LIBZ:\t" << (cv->features&CURL_VERSION_LIBZ ? "YES" : "NO") << endl;
    }else{
        log::error() << "Curl detection failed." << endl;
        throw;
    }
    cout << endl;
    // end curl info 
}

int main(int ac, char *av[])
{
    po::options_description generic("Generic options");
    generic.add_options()
        ("config,c",  po::value<string>(), "use specified config directory")
        ("version,v", "print version information")
        ("help,h",    "print this message")
        ;
    po::options_description cmdline_options;
    cmdline_options.add(generic);

    po::options_description visible("playdar configuration");
    visible.add(generic);

    po::variables_map vm;
    bool error;
    try {
        po::parsed_options parsedopts_cmd = po::command_line_parser(ac, av).options(cmdline_options).run();
        store(parsedopts_cmd, vm);
        error = false;
    } catch (po::error& ex) {
        // probably an unknown option.
        cerr << ex.what() << "\n";
        error = true;
    }
        
    notify(vm);

    if (error || vm.count("help")) {
        cout << visible << endl;
        return error ? 1 : 0;
    }
    if (vm.count("version")) {
        cout << VERSION << endl
             << "Compiled: " << __DATE__ << " " << __TIME__ << endl;
        return 0;
    }
    
    string configdir, configfile;
    
    if( vm.count("config") )
    {
        configdir = vm["config"].as<string>();
    }else{
        configdir = find_config_dir();
    }
    if( !boost::filesystem::is_directory( configdir ) )
    {
        cerr << "config directory not found: " << configdir << endl;
        return -5;
    }
    configfile = configdir += "/playdar.conf";
    if( !boost::filesystem::exists(configfile) )
    {
        cerr << "Config file not found: " << configfile << endl;
        return 1;
    }
    
    try 
    {
        log::info() << "Using config file: " << configfile << endl;
        Config conf(configfile);
        if(conf.get<string>("name", "YOURNAMEHERE")=="YOURNAMEHERE")
        {
            log::info() << "Autodetecting name: " << conf.name() << endl;
        }

#ifndef WIN32
        //TODO we need a shutdown/signal handler for windows too.
        struct sigaction setmask;
        sigemptyset( &setmask.sa_mask );
        setmask.sa_handler = sigfunc;
        setmask.sa_flags   = 0;
        sigaction( SIGHUP,  &setmask, (struct sigaction *) NULL );
        sigaction( SIGINT,  &setmask, (struct sigaction *) NULL );
#endif

        try
        {
            curl_global_init( CURL_GLOBAL_ALL );
            print_curl_info();
        }
        catch(...)
        {
            log::error() << "Curl FAIL." << endl;
            return 9;
        }
        
        app = new MyApplication(conf);
        // start http server:
        string ip = "0.0.0.0"; 
        boost::thread http_thread(
            boost::bind( &start_http_server, 
                         ip, app->conf()->get<int>("http_port", 60210),
                         app->conf()->get<int>("http_threads", boost::thread::hardware_concurrency()+1),
                         app )
            );
        
        http_thread.join();
        log::info() << "HTTP server finished, destructing app..." << endl;
        delete app;
        log::info() << "App deleted." << endl;
        return 0;
    }
    catch(exception& e)
    {
        log::error() << "Playdar main exception: " << e.what() << "\n";
        return 1;
    }

   return 0;
}
