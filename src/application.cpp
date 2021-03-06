/*
 * Copyright (c) 2012, 2013 Aldebaran Robotics. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the COPYING file.
 */

#include <fstream>
#include <cstdlib>

#include <qi/application.hpp>
#include <qi/os.hpp>
#include <qi/log.hpp>
#include <qi/path.hpp>
#include <src/sdklayout.hpp>
#include <numeric>
#include <boost/filesystem.hpp>
#include <boost/thread.hpp>
#include <boost/asio.hpp>

#include "filesystem.hpp"
#include "utils.hpp"
#include "path_conf.hpp"

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#ifdef _WIN32
#include <windows.h>
#endif

qiLogCategory("qi.Application");

namespace bfs = boost::filesystem;

namespace qi {
  static int         globalArgc = -1;
  static char**      globalArgv = 0;
  static bool        globalInitialized = false;
  static bool        globalTerminated = false;

  static std::string globalName;
  static std::vector<std::string>* globalArguments;
  static std::string globalPrefix;
  static std::string globalProgram;
  static std::string globalRealProgram;

  typedef std::vector<boost::function<void()> > FunctionList;
  static FunctionList* globalAtExit = 0;
  static FunctionList* globalAtEnter = 0;
  static FunctionList* globalAtStop = 0;


  static boost::condition_variable globalCond;

  static boost::asio::io_service*             globalIoService = 0;
  static boost::thread*                       globalIoThread = 0;
  static boost::asio::io_service::work*       globalIoWork = 0;
  static std::list<boost::asio::signal_set*>* globalSignalSet = 0;


  static void readPathConf()
  {
    std::string prefix = ::qi::path::sdkPrefix();
    std::set<std::string> toAdd =  ::qi::path::detail::parseQiPathConf(prefix);
    std::set<std::string>::const_iterator it;
    for (it = toAdd.begin(); it != toAdd.end(); ++it) {
      ::qi::path::detail::addOptionalSdkPrefix(it->c_str());
    }
  }

  static void stop_io_service()
  {
    qiLogVerbose() << "Unregistering all signal handlers.";
    //dont call ioservice->stop, just remove all events for the ioservice
    //deleting the object holding the run() method from quitting
    delete globalIoWork;
    globalIoWork = 0;

    if (globalSignalSet) {
      std::list<boost::asio::signal_set*>::iterator it;
      for (it = globalSignalSet->begin(); it != globalSignalSet->end(); ++it) {
        (*it)->cancel();
        delete *it;
      }
      delete globalSignalSet;
      globalSignalSet = 0;
    }
    if (globalIoThread) {
      //wait for the ioservice to terminate
      if (boost::this_thread::get_id() != globalIoThread->get_id())
        globalIoThread->join();
      //we are sure run has stopped so we can delete the io service
      delete globalIoService;
      delete globalIoThread;
      globalIoThread = 0;
      globalIoService = 0;
    }
  }

  static void run_io_service()
  {
    qi::os::setCurrentThreadName("appioservice");
    globalIoService->run();
  }

  static void stop_handler(int signal_number) {
    static int  count_int = 0;
    static int count_term = 0;
    int sigcount = 0;
    if (signal_number == SIGINT) {
      count_int++;
      sigcount = count_int;
    }
    else if (signal_number == SIGTERM) {
      count_term++;
      sigcount = count_term;
    }
    switch (sigcount) {
      case 1:
        qiLogInfo() << "Sending the stop command...";
        //register the signal again to call exit the next time if stop did not succeed
        Application::atSignal(boost::bind<void>(&stop_handler, _1), signal_number);
        // Stop might immediately trigger application destruction, so it has
        // to go after atSignal.
        Application::stop();
        return;
      default:
        //even for SIGTERM this is an error, so return 1.
        qiLogInfo() << "signal " << signal_number << " received a second time, calling exit(1).";
        exit(1);
        return;
    }
  }

  static void signal_handler(const boost::system::error_code& error, int signal_number, boost::function<void (int)> fun)
  {
    //when cancel is called the signal handler is raised with an error. catch it!
    if (!error) {
      fun(signal_number);
    }
  }

  bool Application::atSignal(boost::function<void (int)> func, int signal)
  {
    if (!globalIoService)
    {
      globalIoService = new boost::asio::io_service;
      // Prevent run from exiting
      globalIoWork = new boost::asio::io_service::work(*globalIoService);
      // Start io_service in a thread. It will call our handlers.
      globalIoThread = new boost::thread(&run_io_service);
      // We want signal handlers to work as late as possible.
      ::atexit(&stop_io_service);
      globalSignalSet = new std::list<boost::asio::signal_set*>;
    }

    boost::asio::signal_set *sset = new boost::asio::signal_set(*globalIoService, signal);
    sset->async_wait(boost::bind(signal_handler, _1, _2, func));
    globalSignalSet->push_back(sset);
    return true;
  }

  template<typename T> static T& lazyGet(T* & ptr)
  {
    if (!ptr)
      ptr = new T;
    return *ptr;
  }

  static boost::filesystem::path system_absolute(
      const boost::filesystem::path path)
  {
    if (path.empty())
      return path;

    if (path.is_absolute())
      return path;

    if (path.has_parent_path())
      return bfs::system_complete(path);

    if (!bfs::exists(path) || bfs::is_directory(path))
    {
      std::string envPath = qi::os::getenv("PATH");
      size_t begin = 0;
#ifndef _WIN32
      static const char SEPARATOR = ':';
#else
      static const char SEPARATOR = ';';
#endif
      for (size_t end = envPath.find(SEPARATOR, begin);
          end != std::string::npos;
          begin = end + 1, end = envPath.find(SEPARATOR, begin))
      {
        std::string realPath = envPath.substr(begin, end - begin);
        bfs::path p(realPath);

        p /= path;
        p = boost::filesystem::system_complete(p);

        if (boost::filesystem::exists(p) &&
            !boost::filesystem::is_directory(p))
          return p.string(qi::unicodeFacet());
      }
    }

    // fallback to something
    return bfs::system_complete(path);
  }

  static std::string guess_app_from_path(const char* path)
  {
    boost::filesystem::path execPath(path, qi::unicodeFacet());
    return system_absolute(execPath).make_preferred()
      .string(qi::unicodeFacet());
  }

  static void initApp(int& argc, char ** &argv, const std::string& path)
  {
    // this must be initialized first because readPathConf uses it (through
    // sdklayout)
    if (!path.empty())
    {
      globalProgram = path;
      qiLogVerbose() << "Program path explicitely set to " << globalProgram;
    }
    else
    {
      globalProgram = guess_app_from_path(argv[0]);
      qiLogVerbose() << "Program path guessed as " << globalProgram;
    }

    globalProgram = detail::normalizePath(globalProgram);

    readPathConf();
    if (globalInitialized)
      throw std::logic_error("Application was already initialized");
    globalInitialized = true;
    globalArgc = argc;
    globalArgv = argv;
    std::vector<std::string>& args = lazyGet(globalArguments);
    args.clear();
    for (int i=0; i<argc; ++i)
      args.push_back(argv[i]);

    FunctionList& fl = lazyGet(globalAtEnter);
    qiLogDebug() << "Executing " << fl.size() << " atEnter handlers";
    for (FunctionList::iterator i = fl.begin(); i!= fl.end(); ++i)
      (*i)();
    fl.clear();
    argc = Application::argc();
    argv = globalArgv;
  }

  Application::Application(int& argc, char ** &argv, const std::string& name,
      const std::string& path)
  {
    globalName = name;
    initApp(argc, argv, path);
  }

  Application::Application(const std::string &name, int& argc, char ** &argv)
  {
    globalName = name;
    initApp(argc, argv, "");
  }

  void* Application::loadModule(const std::string& moduleName, int flags)
  {
    void* handle = os::dlopen(moduleName.c_str(), flags);
    if (!handle)
    {
      qiLogVerbose() << "dlopen failed with " << os::dlerror();
    }
    else
    {
      qiLogDebug() << "Loadmodule " << handle;
    }
    // Reprocess atEnter list in case the module had AT_ENTER
    FunctionList& fl = lazyGet(globalAtEnter);
    qiLogDebug() << "Executing " << fl.size() << " atEnter handlers";
    for (FunctionList::iterator i = fl.begin(); i!= fl.end(); ++i)
      (*i)();
    fl.clear();
    return handle;
  }

  void Application::unloadModule(void* handle)
  {
    os::dlclose(handle);
  }

  Application::~Application()
  {
    FunctionList& fl = lazyGet(globalAtExit);
    for (FunctionList::iterator i = fl.begin(); i!= fl.end(); ++i)
      (*i)();
    globalCond.notify_all();
    globalTerminated = true;
  }

  static void initSigIntSigTermCatcher() {
    static bool signalInit = false;

    if (!signalInit) {
      qiLogVerbose() << "Registering SIGINT/SIGTERM handler within qi::Application";
      // kill with no signal sends TERM, control-c sends INT.
      Application::atSignal(boost::bind(&stop_handler, _1), SIGTERM);
      Application::atSignal(boost::bind(&stop_handler, _1), SIGINT);
      signalInit = true;
    }
  }

  void Application::run()
  {
    //run is called, so we catch sigint/sigterm, the default implementation call Application::stop that
    //will make this loop exit.
    initSigIntSigTermCatcher();

    // We just need a barrier, so no need to share the mutex
    boost::mutex m;
    boost::unique_lock<boost::mutex> l(m);
    globalCond.wait(l);
  }

  void Application::stop()
  {
    FunctionList& fl = lazyGet(globalAtStop);
    qiLogDebug() << "Executing " << fl.size() << " atStop handlers";
    for (FunctionList::iterator i = fl.begin(); i!= fl.end(); ++i)
      (*i)();
    globalCond.notify_all();
  }

  void Application::setName(const std::string &name)
  {
    globalName = name;
  }

  std::string Application::name()
  {
    return globalName;
  }

  void Application::setArguments(const std::vector<std::string>& args)
  {
    globalArgc = static_cast<int>(args.size());
    lazyGet(globalArguments) = args;
    globalArgv = new char*[args.size() + 1];
    for (unsigned i=0; i<args.size(); ++i)
      globalArgv[i] = strdup(args[i].c_str());
    globalArgv[args.size()] = 0;
  }

  void Application::setArguments(int argc, char** argv)
  {
    globalArgc = argc;
    globalArgv = argv;
    std::vector<std::string>& args = lazyGet(globalArguments);
    args.resize(argc);
    for (int i=0; i<argc; ++i)
      args[i] = argv[i];
  }

  bool Application::initialized()
  {
    return globalInitialized;
  }

  bool Application::terminated()
  {
    return globalTerminated;
  }

  int Application::argc()
  {
    return globalArgc;
  }

  const char** Application::argv()
  {
    return (const char**)globalArgv;
  }

  bool Application::atEnter(boost::function<void()> func)
  {
    qiLogDebug() << "atEnter";
    lazyGet(globalAtEnter).push_back(func);
    return true;
  }

  bool Application::atExit(boost::function<void()> func)
  {
    lazyGet(globalAtExit).push_back(func);
    return true;
  }

  bool Application::atStop(boost::function<void()> func)
  {
    //If the client call atStop, it mean it will handle the proper destruction
    //of the program by itself. So here we catch SigInt/SigTerm to call Application::stop
    //and let the developer properly stop the application as needed.
    initSigIntSigTermCatcher();
    lazyGet(globalAtStop).push_back(func);
    return true;
  }

  const std::vector<std::string>& Application::arguments()
  {
    return lazyGet(globalArguments);
  }

  const char *Application::program()
  {
    return globalProgram.c_str();
  }

/*
  http://stackoverflow.com/questions/1023306/finding-current-executables-path-without-proc-self-exe
  Some OS-specific interfaces:
  Mac OS X: _NSGetExecutablePath() (man 3 dyld)
  Linux   : readlink /proc/self/exe
  Solaris : getexecname()
  FreeBSD : sysctl CTL_KERN KERN_PROC KERN_PROC_PATHNAME -1
  BSD with procfs: readlink /proc/curproc/file
  Windows : GetModuleFileName() with hModule = NULL

  The portable (but less reliable) method is to use argv[0].
  Although it could be set to anything by the calling program,
  by convention it is set to either a path name of the executable
  or a name that was found using $PATH.

  Some shells, including bash and ksh, set the environment variable "_"
  to the full path of the executable before it is executed. In that case
  you can use getenv("_") to get it. However this is unreliable because
  not all shells do this, and it could be set to anything or be left over
  from a parent process which did not change it before executing your program.
*/
  const char *Application::realProgram()
  {
    try
    {
      if (!globalRealProgram.empty())
        return globalRealProgram.c_str();

#ifdef __APPLE__
      {
        char *fname = (char *)malloc(PATH_MAX);
        uint32_t sz = PATH_MAX;
        fname[0] = 0;
        int ret;
        ret = _NSGetExecutablePath(fname, &sz);
        if (ret == 0)
        {
          globalRealProgram = fname;
          globalRealProgram = detail::normalizePath(globalRealProgram);
        }
        else
        {
          globalRealProgram = guess_app_from_path(::qi::Application::argv()[0]);
        }
        free(fname);
      }
#elif __linux__
      boost::filesystem::path p("/proc/self/exe");
      boost::filesystem::path fname = boost::filesystem::read_symlink(p);

      if (!boost::filesystem::is_empty(fname))
        globalRealProgram = fname.string().c_str();
      else
        globalRealProgram = guess_app_from_path(::qi::Application::argv()[0]);
#elif _WIN32
      WCHAR fname[MAX_PATH];
      int ret = GetModuleFileNameW(NULL, fname, MAX_PATH);
      if (ret > 0)
      {
        fname[ret] = '\0';
        boost::filesystem::path programPath(fname, qi::unicodeFacet());
        globalRealProgram = programPath.string(qi::unicodeFacet());
      }
      else
      {
        // GetModuleFileName failed, trying to guess from argc, argv...
        globalRealProgram = guess_app_from_path(::qi::Application::argv()[0]);
      }
#else
      globalRealProgram = guess_app_from_path(::qi::Application::argv()[0]);
#endif
      return globalRealProgram.c_str();
    }
    catch (...)
    {
      return NULL;
    }
  }
}
