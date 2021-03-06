/*
 * Copyright (c) 2012 Aldebaran Robotics. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the COPYING file.
 */

#include <qi/log.hpp>
#include "log_p.hpp"
#include <qi/os.hpp>
#include <list>
#include <map>
#include <cstring>
#include <iomanip>

#include <qi/application.hpp>
#include <qi/atomic.hpp>
#include <qi/log/consoleloghandler.hpp>

#include <boost/thread/thread.hpp>
#include <boost/thread/condition_variable.hpp>
#include <boost/thread/recursive_mutex.hpp>
#include <boost/program_options.hpp>
#include <boost/unordered_map.hpp>

#ifndef ANDROID
# include <boost/lockfree/queue.hpp>
#endif
#include <boost/function.hpp>

#ifdef ANDROID
# include <android/log.h>
#endif

#ifndef _WIN32
#include <fnmatch.h>
#else
# include <shlwapi.h>
# pragma comment(lib, "shlwapi.lib")
#endif


#define RTLOG_BUFFERS (128)

#define CAT_SIZE 64
#define FILE_SIZE 128
#define FUNC_SIZE 64
#define LOG_SIZE 2048

qiLogCategory("qi.log");

namespace qi {
  namespace detail {

    std::string logline(LogContext         context,
                        const              os::timeval date,
                        const char        *category,
                        const char        *msg,
                        const char        *file,
                        const char        *fct,
                        const int          line,
                        const qi::LogLevel verb)
    {
      std::stringstream logline;

      if (context & qi::LogContextAttr_Verbosity)
        logline << qi::log::logLevelToString(verb) << " ";
      if (context & qi::LogContextAttr_ShortVerbosity)
        logline << qi::log::logLevelToString(verb, false) << " ";
      if (context & qi::LogContextAttr_Date)
        logline << qi::detail::dateToString(date) << " ";
      if (context & qi::LogContextAttr_Tid)
        logline << qi::detail::tidToString() << " ";
      if (context & qi::LogContextAttr_Category)
        logline << category << ": ";
      if (context & qi::LogContextAttr_File) {
        logline << file;
        if (line != 0)
          logline << "(" << line << ")";
        logline << " ";
      }
      if (context & qi::LogContextAttr_Function)
        logline << fct << "() ";
      if (context & qi::LogContextAttr_Return)
        logline << std::endl;
      logline.write(msg, qi::detail::rtrim(msg));
      logline << std::endl;

      return logline.str();
    }

    const std::string dateToString(const qi::os::timeval date)
    {
      std::stringstream ss;
      ss << date.tv_sec << "."
        << std::setw(6) << std::setfill('0') << date.tv_usec;

      return ss.str();
    }

    const std::string tidToString()
    {
      int tid = qi::os::gettid();
      std::stringstream ss;
      ss << tid;

      return ss.str();
    }

    /* Emulate previous behavior that ensured a single newline was
    * present at the end on message.
    */
    int rtrim(const char *msg)
    {
      size_t p = strlen(msg) - 1;

      p -= (msg[p] == '\r')? 1:
             (msg[p] == '\n')?
               (p && msg[p-1] == '\r')? 2:1
               :0;

      return p+1;
    }
  }

  namespace log {

    typedef struct sPrivateLog
    {
      qi::LogLevel    _logLevel;
      char            _category[CAT_SIZE];
      char            _file[FILE_SIZE];
      char            _function[FUNC_SIZE];
      int             _line;
      char            _log[LOG_SIZE];
      qi::os::timeval _date;
    } privateLog;

    class Log
    {
    public:
      inline Log();
      inline ~Log();

      struct Handler
      {
        logFuncHandler func;
        unsigned int   index; // index of this handler in category levels
      };

      void run();
      void printLog();
      // Invoke handlers who enabled given level/category
      void dispatch(const qi::LogLevel,
                    const qi::os::timeval,
                    const char*,
                    const char*,
                    const char*,
                    const char*,
                    int);
      void dispatch(const qi::LogLevel level,
                    const qi::os::timeval date,
                    detail::Category& category,
                    const char* log,
                    const char* file,
                    const char* function,
                    int line);
      Handler* logHandler(SubscriberId id);

      void setSynchronousLog(bool sync);
    public:
      bool                       LogInit;
      boost::thread              LogThread;
      boost::mutex               LogWriteLock;
      boost::mutex               LogHandlerLock;
      boost::condition_variable  LogReadyCond;
      bool                       SyncLog;
      bool                       AsyncLogInit;

#ifndef ANDROID
      boost::lockfree::queue<privateLog*>     logs;
#endif

      typedef std::map<std::string, Handler> LogHandlerMap;
      LogHandlerMap logHandlers;

      qi::Atomic<int> nextIndex;
    };

    // If we receive a setLevel with a globbing category, we must keep it
    // in mind, in case a new category that matches the glob is created.
    struct GlobRule
    {
      GlobRule(std::string t, unsigned int i, qi::LogLevel l)
      : target(t)
      , id(i)
      , level(l)
      {}

      bool matches(const std::string& n) const
      {
        return os::fnmatch(target, n);
      }

      std::string  target;       // glob target
      unsigned int id;           // listener id or -1 for all
      qi::LogLevel level;
    };

    static std::vector<GlobRule> _glGlobRules;

    // categories must be accessible at static init: cannot go in Log class
    typedef std::map<std::string, detail::Category*> CategoryMap;
    static CategoryMap* _glCategories = 0;
    inline CategoryMap& _categories()
    {
      if (!_glCategories)
        _glCategories = new CategoryMap;
      return *_glCategories;
    }

    // protects globs and categories, both the map and the per-category vector
    static boost::recursive_mutex          *_glMutex   = 0;
    inline boost::recursive_mutex& _mutex()
    {
      if (!_glMutex)
        _glMutex = new boost::recursive_mutex();
      return *_glMutex;
    }

    static int                    _glContext = 0;
    static bool                   _glInit    = false;
    static ConsoleLogHandler     *_glConsoleLogHandler;
    static LogColor               _glColorWhen = LogColor_Auto;

    static Log                   *LogInstance;
    static privateLog             LogBuffer[RTLOG_BUFFERS];
    static volatile unsigned long LogPush = 0;

    namespace detail {

      // This pattern allows to continue logging at static destruction time
      // even if the static FormatMap is destroyed
      class FormatMap: public boost::unordered_map<std::string, boost::format>
      {
      public:
        FormatMap(bool& ward)
        : ward_(ward)
        {
          ward_ = true;
        }

        ~FormatMap()
        {
          ward_ = false;
        }

      private:
        bool& ward_;
      };

      boost::format getFormat(const std::string& s)
      {
        static bool map_ok(false);
        static FormatMap map(map_ok);
        if (map_ok)
        {
          static boost::mutex mutex;
          boost::mutex::scoped_lock lock(mutex);
          FormatMap::iterator i = map.find(s);
          if (i == map.end())
          {
            boost::format& result = map[s]; // creates with default ctor
            result.parse(s);
            result.exceptions(boost::io::no_error_bits);
            return result;
          }
          else
            return i->second;
        }
        else
          {
            boost::format result = boost::format(s);
            result.exceptions(boost::io::no_error_bits);
            return result;
          }
      }
    }

    namespace detail {
      void Category::setLevel(SubscriberId sub, qi::LogLevel level)
      {
        boost::recursive_mutex::scoped_lock lock(_mutex());
        if (levels.size() <= sub)
        {
          bool willUseDefault = (levels.size() < sub);
          levels.resize(sub + 1, LogLevel_Info);
          if (willUseDefault)
          { // should not happen
            // cannot qilog here or deadlock
            std::cerr << "Default level for category " << name
              << " will be used for subscriber " << sub
              << ", use setVerbosity() after adding the subscriber"
              << std::endl;
          }
        }
        levels[sub] = level;
        maxLevel = *std::max_element(levels.begin(), levels.end());
      }
    }

    // check and apply existing glob if they match given category
    static void checkGlobs(detail::Category* cat)
    {
      boost::recursive_mutex::scoped_lock lock(_mutex());
      for (unsigned i=0; i<_glGlobRules.size(); ++i) {
        GlobRule& g = _glGlobRules[i];
        if (g.matches(cat->name))
          cat->setLevel(g.id, g.level);
      }
    }

    // apply a globbing rule to existing categories
    static void applyGlob(const GlobRule& g)
    {
      boost::recursive_mutex::scoped_lock lock(_mutex());
      CategoryMap& c = _categories();
      for (CategoryMap::iterator it = c.begin(); it != c.end(); ++it)
      {
        assert(it->first == it->second->name);
        if (g.matches(it->first)) {
          detail::Category* cat = it->second;
          checkGlobs(cat);
        }
      }
    }

    // Check if globRule replaces an existiing one, then replace or append
    static void mergeGlob(const GlobRule& p)
    {
      boost::recursive_mutex::scoped_lock lock(_mutex());
      for (unsigned i=0; i<_glGlobRules.size(); ++i)
      {
        GlobRule& c = _glGlobRules[i];
        if (p.target == c.target && p.id == c.id)
        {
          c = p;
          return;
        }
      }
      _glGlobRules.push_back(p);
    }

    static class DefaultLogInit
    {
    public:
      DefaultLogInit()
      {
        _glInit = false;
        qi::log::init();
      }

      ~DefaultLogInit()
      {
        qi::log::destroy();
      };
    } synchLog;

    void Log::printLog()
    {
// Logs are handled in qi::log in Android
#ifndef ANDROID
      privateLog* pl = 0;
      boost::mutex::scoped_lock lock(LogHandlerLock);
      while (logs.pop(pl))
      {
        dispatch(pl->_logLevel,
                 pl->_date,
                 pl->_category,
                 pl->_log,
                 pl->_file,
                 pl->_function,
                 pl->_line);
      }
#endif
    }

    void Log::dispatch(const qi::LogLevel level,
                       const qi::os::timeval date,
                       const char*  category,
                       const char* log,
                       const char* file,
                       const char* function,
                       int line)
    {
      dispatch(level, date, *addCategory(category), log, file, function, line);
    }

    void Log::dispatch(const qi::LogLevel level,
                       const qi::os::timeval date,
                       detail::Category& category,
                       const char* log,
                       const char* file,
                       const char* function,
                       int line)
    {
      boost::recursive_mutex::scoped_lock lock(_mutex());
      if (!logHandlers.empty())
      {
        LogHandlerMap::iterator it;
        for (it = logHandlers.begin(); it != logHandlers.end(); ++it)
        {
          Handler& h = it->second;
          unsigned int index = h.index;
          if (category.levels.size() <= index || category.levels[index] >= level)
            h.func(level, date, category.name.c_str(), log, file, function, line);
        }
      }
    }

    void Log::run()
    {
      while (LogInit)
      {
        {
          boost::mutex::scoped_lock lock(LogWriteLock);
          LogReadyCond.wait(lock);
        }

        printLog();
      }
    }

    void Log::setSynchronousLog(bool sync)
    {
      SyncLog = sync;
      if (!SyncLog && !AsyncLogInit)
      {
        AsyncLogInit = true;
        LogThread = boost::thread(&Log::run, this);
      }
    };

    inline Log::Log() :
      SyncLog(true),
      AsyncLogInit(false)
#ifndef ANDROID
      , logs(50)
#endif
    {
      LogInit = true;
    };

    inline Log::~Log()
    {
      if (!LogInit)
        return;
      LogInit = false;

      if (AsyncLogInit)
      {
        LogThread.interrupt();
        LogThread.join();

        printLog();
      }
    }

    static void my_strcpy(char *dst, const char *src, int len) {
      if (!src)
        src = "(null)";
#ifdef _MSV_VER
      strncpy_s(dst, len, src, _TRUNCATE);
#else
      strncpy(dst, src, len);
      dst[len - 1] = 0;
#endif
    }

    static void doInit() {
      //if init has already been called, we are set here. (reallocating all globals
      // will lead to racecond)
      if (_glInit)
        return;
      _glConsoleLogHandler = new ConsoleLogHandler;
      LogInstance          = new Log;
      addLogHandler("consoleloghandler",
                    boost::bind(&ConsoleLogHandler::log,
                                _glConsoleLogHandler,
                                _1, _2, _3, _4, _5, _6, _7));
      _glInit = true;
    }

    void init(qi::LogLevel verb,
              int ctx,
              bool synchronous)
    {
      setLogLevel(verb);
      setContext(ctx);

      QI_ONCE(doInit());

      setSynchronousLog(synchronous);
    }

    void destroy()
    {
      if (!_glInit)
        return;
      _glInit = false;
      LogInstance->printLog();
      delete _glConsoleLogHandler;
      _glConsoleLogHandler = 0;
      delete LogInstance;
      LogInstance = 0;
    }

    void flush()
    {
      if (_glInit)
        LogInstance->printLog();
    }

    void log(const qi::LogLevel    verb,
             CategoryType          category,
             const std::string&    msg,
             const char           *file,
             const char           *fct,
             const int             line)
    {
#ifndef ANDROID
      if (LogInstance->SyncLog)
      {
        if (!detail::isVisible(category, verb))
          return;
        qi::os::timeval tv;
        qi::os::gettimeofday(&tv);
        LogInstance->dispatch(verb, tv, *category, msg.c_str(), file, fct, line);
      }
      else
#endif
      // FIXME suboptimal
      // log is also a qi namespace, this line confuses some compilers if
      // namespace is not explicit
      ::qi::log::log(verb, category->name.c_str(), msg.c_str(), file, fct, line);
    }

    void log(const qi::LogLevel    verb,
             const char           *category,
             const char           *msg,
             const char           *file,
             const char           *fct,
             const int             line)
    {
      if (!isVisible(category, verb))
        return;

#ifdef ANDROID
      std::map<LogLevel, android_LogPriority> _conv;

      _conv[silent]  = ANDROID_LOG_SILENT;
      _conv[fatal]   = ANDROID_LOG_FATAL;
      _conv[error]   = ANDROID_LOG_ERROR;
      _conv[warning] = ANDROID_LOG_WARN;
      _conv[info]    = ANDROID_LOG_INFO;
      _conv[verbose] = ANDROID_LOG_VERBOSE;
      _conv[debug]   = ANDROID_LOG_DEBUG;

      __android_log_print(_conv[verb], category, msg);
#else
      if (!LogInstance)
        return;
      if (!LogInstance->LogInit)
        return;

      qi::os::timeval tv;
      qi::os::gettimeofday(&tv);
      if (LogInstance->SyncLog)
      {
        LogInstance->dispatch(verb, tv, category, msg, file, fct, line);
      }
      else
      {
        int tmpRtLogPush = ++LogPush % RTLOG_BUFFERS;
        privateLog* pl = &(LogBuffer[tmpRtLogPush]);



        pl->_logLevel = verb;
        pl->_line = line;
        pl->_date.tv_sec = tv.tv_sec;
        pl->_date.tv_usec = tv.tv_usec;

        my_strcpy(pl->_category, category, CAT_SIZE);
        my_strcpy(pl->_file, file, FILE_SIZE);
        my_strcpy(pl->_function, fct, FUNC_SIZE);
        my_strcpy(pl->_log, msg, LOG_SIZE);
        LogInstance->logs.push(pl);
        LogInstance->LogReadyCond.notify_one();
      }
#endif
    }

    Log::Handler* Log::logHandler(SubscriberId id)
    {
       boost::mutex::scoped_lock l(LogInstance->LogHandlerLock);
       LogHandlerMap::iterator it;
       for (it = logHandlers.begin(); it != logHandlers.end(); ++it)
       {
         if (it->second.index == id)
           return &it->second;
       }
       return 0;
    }

    SubscriberId addLogHandler(const std::string& name, logFuncHandler fct,
                               qi::LogLevel defaultLevel)
    {
      if (!LogInstance)
        return -1;
      boost::mutex::scoped_lock l(LogInstance->LogHandlerLock);
      unsigned int id = ++LogInstance->nextIndex;
      --id; // no postfix ++ on atomic
      Log::Handler h;
      h.index = id;
      h.func = fct;
      LogInstance->logHandlers[name] = h;
      setLogLevel(defaultLevel, id);
      return id;
    }

    void removeLogHandler(const std::string& name)
    {
      if (!LogInstance)
        return;
      boost::mutex::scoped_lock l(LogInstance->LogHandlerLock);
      LogInstance->logHandlers.erase(name);
    }

    qi::LogLevel stringToLogLevel(const char* verb)
    {
      std::string v(verb);
      if (v == "silent" || v == "0")
        return qi::LogLevel_Silent;
      if (v == "fatal" || v == "1")
        return qi::LogLevel_Fatal;
      if (v == "error" || v == "2")
        return qi::LogLevel_Error;
      if (v == "warning" || v == "3")
        return qi::LogLevel_Warning;
      if (v == "info" || v == "4")
        return qi::LogLevel_Info;
      if (v == "verbose" || v == "5")
        return qi::LogLevel_Verbose;
      if (v == "debug" || v == "6")
        return qi::LogLevel_Debug;
      return qi::LogLevel_Info;
    }

    const char *logLevelToString(const qi::LogLevel level, bool verbose)
    {
      static const char *sverb[] = {
        "[SILENT]", // never shown
        "[F]",
        "[E]",
        "[W]",
        "[I]",
        "[V]",
        "[D]"
      };
      static const char *verb[] = {
        "[SILENT]", // never shown
        "[FATAL]",
        "[ERROR]",
        "[WARN ]",
        "[INFO ]",
        "[VERB ]",
        "[DEBUG]"
      };
      if (level < 0 || level > qi::LogLevel_Debug)
        return "Invalid log level";
      if (verbose)
        return verb[level];
      return sverb[level];
    }

    qi::LogLevel logLevel(SubscriberId sub)
    {
      CategoryType cat = addCategory("*");
      if (sub < cat->levels.size())
        return cat->levels[sub];
      return LogLevel_Info;
    }

    void setContext(int ctx)
    {
      _glContext = ctx;
      qiLogVerbose() << "Context set to " << _glContext;
    };

    int context()
    {
      return _glContext;
    };

    void setColor(LogColor color)
    {
      _glColorWhen = color;
      _glConsoleLogHandler->updateColor();
    };

    LogColor color()
    {
      return _glColorWhen;
    }

    void setSynchronousLog(bool sync)
    {
      LogInstance->setSynchronousLog(sync);
    };

    CategoryType addCategory(const std::string& name)
    {
      boost::recursive_mutex::scoped_lock lock(_mutex());
      CategoryMap& c = _categories();
      CategoryMap::iterator i = c.find(name);
      if (i == c.end())
      {
        detail::Category* res = new detail::Category(name);
        c[name] = res;
        checkGlobs(res);
        return res;
      }
      else
        return i->second;
    }

    bool isVisible(const std::string& category, qi::LogLevel level)
    {
      return log::isVisible(addCategory(category), level);
    }

    bool isVisible(CategoryType category, qi::LogLevel level)
    {
      return detail::isVisible(category, level);
    }

    void enableCategory(const std::string& cat, SubscriberId sub)
    {
      addFilter(cat, logLevel(sub), sub);
    }

    void disableCategory(const std::string& cat, SubscriberId sub)
    {
      addFilter(cat, LogLevel_Silent, sub);
    }

    void addFilter(const std::string& catName, qi::LogLevel level, SubscriberId sub)
    {
      qiLogVerbose() << "setCategory(cat=" << catName << ", level=" << (int)level << ", sub=" << (int)sub << ")";
      if (catName.find('*') != catName.npos)
      {
        GlobRule rule(catName, sub, level);
        mergeGlob(rule);
        applyGlob(rule);
      }
      else
      {
        CategoryType cat = addCategory(catName);
        cat->setLevel(sub, level);
        GlobRule rule(catName, sub, level);
        mergeGlob(rule);
      }
    }

    std::vector<std::string> categories()
    {
      boost::recursive_mutex::scoped_lock lock(_mutex());
      std::vector<std::string> res;
      CategoryMap& c = _categories();
      for (CategoryMap::iterator it = c.begin(); it != c.end(); ++it)
        res.push_back(it->first);
      return res;
    }

    void setLogLevel(qi::LogLevel level, SubscriberId sub)
    {
      boost::recursive_mutex::scoped_lock lock(_mutex());
      // Check if there is already a '*' rule, replace it if so
      bool found = false;
      for (unsigned i=0; i<_glGlobRules.size(); ++i)
      {
        if (_glGlobRules[i].target == "*" && _glGlobRules[i].id == sub)
        {
          _glGlobRules[i].level = level;
          found = true;
          break;
        }
      }
      if (!found)
      {
        // Prepend the rule
        GlobRule rule("*", sub, level);
        // Insert the rule with initial '*' rule set, ordered by subscriber id
        // to avoid spurious unset-verbosity warning
        std::vector<GlobRule>::iterator insertIt = _glGlobRules.begin();
        while (insertIt != _glGlobRules.end()
          && insertIt->target == "*" && insertIt->id < sub)
          ++insertIt;
        _glGlobRules.insert(insertIt, rule);
      }
      // Then reprocess all categories
      CategoryMap& c = _categories();
      for (CategoryMap::iterator it = c.begin(); it != c.end(); ++it)
        checkGlobs(it->second);
    }

    void addFilters(const std::string& rules, SubscriberId sub)
    {
      // See doc in header for format
      size_t pos = 0;
      while (true)
      {
        if (pos >= rules.length())
          break;
        size_t next = rules.find(':', pos);
        std::string token;
        if (next == rules.npos)
          token = rules.substr(pos);
        else
          token = rules.substr(pos, next-pos);
        if (token.empty())
        {
          pos = next + 1;
          continue;
        }
        if (token[0] == '+')
          token = token.substr(1);
        size_t sep = token.find('=');
        if (sep != token.npos)
        {
          std::string sLevel = token.substr(sep+1);
          std::string cat = token.substr(0, sep);
          qi::LogLevel level = stringToLogLevel(sLevel.c_str());
          addFilter(cat, level, sub);
        }
        else
        {
          if (token[0] == '-')
            addFilter(token.substr(1), LogLevel_Silent, sub);
          else
            addFilter(token, LogLevel_Debug, sub);
        }
        if (next == rules.npos)
          break;
        pos = next+1;
      }
    }

    static void _setLogLevel(const std::string &level)
    {
      setLogLevel(stringToLogLevel(level.c_str()));
    }

    static void _setColor(const std::string &color)
    {
      if (color == "always")
        setColor(LogColor_Always);
      else if (color == "never")
        setColor(LogColor_Never);
      else
        setColor(LogColor_Auto);
    }

    static void _setFilters(const std::string &filters)
    {
      addFilters(filters);
    }

    static const std::string contextLogOption = ""
        "Show context logs, it's a bit field (add the values below):\n"
        " 1  : Verbosity\n"
        " 2  : ShortVerbosity\n"
        " 4  : Date\n"
        " 8  : ThreadId\n"
        " 16 : Category\n"
        " 32 : File\n"
        " 64 : Function\n"
        " 128: EndOfLine\n"
        "some useful values for context are:\n"
        " 26 : (verb+threadId+cat)\n"
        " 30 : (verb+threadId+date+cat)\n"
        " 126: (verb+threadId+date+cat+file+fun)\n"
        " 254: (verb+threadId+date+cat+file+fun+eol)\n"
        "Can be set with env var QI_LOG_CONTEXT";

    static const std::string levelLogOption = ""
        "Change the log minimum level: [0-6] (default:4)\n"
        " 0: silent\n"
        " 1: fatal\n"
        " 2: error\n"
        " 3: warning\n"
        " 4: info\n"
        " 5: verbose\n"
        " 6: debug\n"
        "Can be set with env var QI_LOG_LEVEL";

    static const std::string filterLogOption = ""
        "Set log filtering options.\n"

        " Colon separated list of rules.\n"
        " Each rule can be:\n"
        "  - +CAT      : enable category CAT\n"
        "  - -CAT      : disable category CAT\n"
        "  - CAT=level : set category CAT to level\n"
        " Each category can include a '*' for globbing.\n"
        "Can be set with env var QI_LOG_FILTERS\n"
        "Example: 'qi.*=debug:-qi.foo:+qi.foo.bar' (all qi.* logs in info, remove all qi.foo logs except qi.foo.bar)";

    _QI_COMMAND_LINE_OPTIONS(
      "Logging options",
      ("qi-log-context",     value<int>()->notifier(&setContext), contextLogOption.c_str())
      ("qi-log-synchronous", bool_switch()->notifier(boost::bind(&setSynchronousLog, true)),  "Activate synchronous logs.")
      ("qi-log-level",       value<std::string>()->notifier(&_setLogLevel), levelLogOption.c_str())
      ("qi-log-color",       value<std::string>()->notifier(&_setColor), "Tell if we should put color or not in log (auto, always, never).")
      ("qi-log-filters",     value<std::string>()->notifier(&_setFilters), filterLogOption.c_str())
    )

    int process_env()
    {
      const char* verbose = std::getenv("QI_LOG_LEVEL");
      if (verbose)
        setLogLevel(stringToLogLevel(verbose));
      const char *context = std::getenv("QI_LOG_CONTEXT");
      if (context)
        _glContext = (atoi(context));
      const char* rules = std::getenv("QI_LOG_FILTERS");
      if (rules)
        addFilters(rules);
      return 0;
    }
    static int _init = process_env();


    // deprecated
    qi::LogLevel verbosity(SubscriberId sub)
    {
      return logLevel(sub);
    }

    // deprecated
    void setVerbosity(qi::LogLevel level, SubscriberId sub)
    {
      setLogLevel(level, sub);
    }

    // deprecated
    void setVerbosity(const std::string& rules, SubscriberId sub)
    {
      addFilters(rules, sub);
    }

    // deprecated
    void setCategory(const std::string& catName, qi::LogLevel level, SubscriberId sub)
    {
      addFilter(catName, level, sub);
    }

  } // namespace log
} // namespace qi

