// References:
//    https://spin.atomicobject.com/2013/01/13/exceptions-stack-traces-c/
//    http://theorangeduck.com/page/printing-stack-trace-mingw
//    https://en.wikipedia.org/wiki/Name_mangling

// In order for this to work w/mingw:
//    - you need to link with Dbghelp.dll
//    - you need to turn on debug symbols with "-g"
//    - you need to have the addr2line tool available (I use the one from cygwin)

// In order for this to work w/macOS:
//    - you need to turn on C/CXX debug symbols with "-g"
//    - you need to turn off C/CXX "Position Independent Executable" with "-fno-pie"
//    - you need to turn off linker "Position Independent Executable" with "-Wl,-no_pie"
//    - it helps to turn off optimization with "-O0"
//    - it helps to always include frame pointers with "-fno-omit-frame-pointer"
//
// Andy Maloney <asmaloney@gmail.com>

#include <stdlib.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStringList>
#include <QTextStream>

#ifdef Q_OS_WIN
#include <windows.h>
#include <imagehlp.h>
# ifndef ASM_CRASH_REPORT_MSVC
# include <cxxabi.h>
# endif
#else
#include <err.h>
#include <execinfo.h>
#include <signal.h>
#endif

#include "asmCrashReport.h"

#define TRACE_MAX_FUNCTION_NAME_LENGTH 1024

namespace asmCrashReport
{
   static QString sProgramName;        // the full path to the executable (which we need to resolve symbols)
   static QString sCrashReportDirPath; // where to store the crash reports

   // Note that we are looking for GCC-style name mangles
   // See: https://en.wikipedia.org/wiki/Name_mangling#How_different_compilers_mangle_the_same_functions
   static QRegularExpression  sSymbolMatching("^.*(_Z[^ ]+).*$");

   static logWrittenCallback  sLogWrittenCallback; // function to call after we've written the log file
   static QProcess            *sProcess = nullptr; // process used to capture output of address mapping tool

   void  _writeLog( const QString &inSignal, const QStringList &inFrameInfoList )
   {
      const QStringList cReportHeader{
         QStringLiteral( "%1 v%2" ).arg( QCoreApplication::applicationName(), QCoreApplication::applicationVersion() ),
               QDateTime::currentDateTime().toString( "dd MMM yyyy @ HH:mm:ss" ),
               QString(),
               inSignal,
               QString(),
      };

      QDir().mkpath( sCrashReportDirPath );

      const QString  cFileName = QStringLiteral( "%1/%2 %3 Crash.log" ).arg( sCrashReportDirPath,
                                                                             QDateTime::currentDateTime().toString( "yyyyMMdd-HHmmss" ),
                                                                             QCoreApplication::applicationName() );

      QFile file( cFileName );
      bool  fileWritten = false;

      if ( file.open( QIODevice::WriteOnly | QIODevice::Text ) )
      {
         QTextStream stream( &file );

         for ( const QString &data : cReportHeader + inFrameInfoList )
         {
            stream << data << endl;
         }

         fileWritten = true;
      }
      file.close();

      if ( sLogWrittenCallback != nullptr )
      {
         (*sLogWrittenCallback)( cFileName, fileWritten );
      }
   }

   // Resolve symbol name & source location
   QString _addr2line( const QString &inProgramName, void const * const inAddr )
   {
#ifndef ASM_CRASH_REPORT_MSVC
      const QString  cAddrStr = QStringLiteral( "0x%1" ).arg( quintptr( inAddr ), 16, 16, QChar( '0' ) );

#ifdef Q_OS_MAC
      const QString  cCommand = QStringLiteral("atos");
      const QStringList lArgs = QStringList() << inProgramName << "-arch" << "x86_64" << cAddrStr;
#else
      const QString  cCommand = QStringLiteral("%1/tools/addr2line").arg(QCoreApplication::applicationDirPath());
      const QStringList lArgs = QStringList() << "-f" << "-p" << "-e" << inProgramName << cAddrStr;
#endif

      sProcess->start( cCommand, lArgs, QIODevice::ReadOnly );

      if ( !sProcess->waitForFinished() )
      {
         return QStringLiteral( "* Error running command\n   %1\n   %2" ).arg( cCommand, sProcess->errorString() );
      }

      const QString  cLocationStr = QString( sProcess->readAll() ).trimmed();

      return (cLocationStr == cAddrStr) ? QString() : cLocationStr;
#else
       HANDLE process = GetCurrentProcess();

       IMAGEHLP_SYMBOL64 *symbol = (IMAGEHLP_SYMBOL64 *)malloc(sizeof(IMAGEHLP_SYMBOL64)+(TRACE_MAX_FUNCTION_NAME_LENGTH - 1) * sizeof(TCHAR));
       symbol->MaxNameLength = TRACE_MAX_FUNCTION_NAME_LENGTH;
       symbol->SizeOfStruct = sizeof(IMAGEHLP_SYMBOL64);
       IMAGEHLP_LINE64 *line = (IMAGEHLP_LINE64 *)malloc(sizeof(IMAGEHLP_LINE64));
       line->SizeOfStruct = sizeof(IMAGEHLP_LINE64);
       char *und_name = (char *)malloc(TRACE_MAX_FUNCTION_NAME_LENGTH);

       QString locationStr = QString("????");
       DWORD64 displacement64;
       DWORD64 offset = DWORD64( quintptr(inAddr) );

       if(SymGetSymFromAddr64(process, offset, &displacement64, symbol))
       {
           DWORD displacement;
           locationStr = QString(symbol->Name);
           if(*symbol->Name != '\0')
           {
               UnDecorateSymbolName(symbol->Name, und_name, TRACE_MAX_FUNCTION_NAME_LENGTH, UNDNAME_COMPLETE);
               locationStr = QString(und_name);
           }

           if (SymGetLineFromAddr64(process, offset, &displacement, line))
           {
               locationStr += QString(" (%1 : %2)")
                                 .arg(QString(line->FileName).split("\\").last())
                                 .arg(line->LineNumber);
           }
       }

       return locationStr;
#endif
   }

#ifdef Q_OS_WIN
   QStringList _stackTrace( CONTEXT* context )
   {
      HANDLE process = GetCurrentProcess();
      HANDLE thread = GetCurrentThread();

      SymInitialize( process, 0, true );
      DWORD symOptions = SymGetOptions();
      symOptions |= SYMOPT_LOAD_LINES | SYMOPT_UNDNAME;
      SymSetOptions(symOptions);

      STACKFRAME64 stackFrame;
      memset( &stackFrame, 0, sizeof( STACKFRAME64 ) );

      DWORD   image;

#ifdef _M_IX86
      image = IMAGE_FILE_MACHINE_I386;

      stackFrame.AddrPC.Offset = context->Eip;
      stackFrame.AddrPC.Mode = AddrModeFlat;
      stackFrame.AddrStack.Offset = context->Esp;
      stackFrame.AddrStack.Mode = AddrModeFlat;
      stackFrame.AddrFrame.Offset = context->Ebp;
      stackFrame.AddrFrame.Mode = AddrModeFlat;
#elif _M_X64
      image = IMAGE_FILE_MACHINE_AMD64;
      stackFrame.AddrPC.Offset = context->Rip;
      stackFrame.AddrPC.Mode = AddrModeFlat;
      stackFrame.AddrFrame.Offset = context->Rsp;
      stackFrame.AddrFrame.Mode = AddrModeFlat;
      stackFrame.AddrStack.Offset = context->Rsp;
      stackFrame.AddrStack.Mode = AddrModeFlat;
#elif _M_IA64
      image = IMAGE_FILE_MACHINE_IA64;
      stackFrame.AddrPC.Offset = context->StIIP;
      stackFrame.AddrPC.Mode = AddrModeFlat;
      stackFrame.AddrstackFrame.Offset = context->IntSp;
      stackFrame.AddrstackFrame.Mode = AddrModeFlat;
      stackFrame.AddrBStore.Offset = context->RsBSP;
      stackFrame.AddrBStore.Mode = AddrModeFlat;
      stackFrame.AddrStack.Offset = context->IntSp;
      stackFrame.AddrStack.Mode = AddrModeFlat;
#else
#error "This platform is not supported."
#endif

      QStringList frameList;
      int         frameNumber = 0;

      while ( StackWalk64(
                 image, process, thread,
                 &stackFrame, context, NULL,
                 SymFunctionTableAccess64, SymGetModuleBase64, NULL )
              )
      {
         QString locationStr = _addr2line( sProgramName, (void*)stackFrame.AddrPC.Offset );

#ifndef ASM_CRASH_REPORT_MSVC
         // match the mangled name and demangle if we can
         QRegularExpressionMatch match = sSymbolMatching.match( locationStr );

         const QString  cSymbol( match.captured( 1 ) );

         if ( !cSymbol.isNull() )
         {
            int   demangleStatus = 0;

            const char  *cFunctionName = abi::__cxa_demangle( cSymbol.toLatin1().constData(), nullptr, nullptr, &demangleStatus);

            if ( demangleStatus == 0 )
            {
               locationStr.replace( cSymbol, cFunctionName );
            }
         }
#endif

         frameList += QStringLiteral( "[%1] 0x%2 %3" )
                      .arg( QString::number( frameNumber ) )
                      .arg( quintptr( (void*)stackFrame.AddrPC.Offset ), 16, 16, QChar( '0' ) )
                      .arg( locationStr );

         ++frameNumber;
         if(frameNumber >= 10)
             break;
      }

      SymCleanup( GetCurrentProcess() );

      return frameList;
   }

   LONG WINAPI _winExceptionHandler( EXCEPTION_POINTERS *inExceptionInfo )
   {
      const QString  cExceptionType = [] ( DWORD code ) {
         switch( code )
         {
            case EXCEPTION_ACCESS_VIOLATION:
               return QStringLiteral( "EXCEPTION_ACCESS_VIOLATION" );
            case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
               return QStringLiteral( "EXCEPTION_ARRAY_BOUNDS_EXCEEDED" );
            case EXCEPTION_BREAKPOINT:
               return QStringLiteral( "EXCEPTION_BREAKPOINT" );
            case EXCEPTION_DATATYPE_MISALIGNMENT:
               return QStringLiteral( "EXCEPTION_DATATYPE_MISALIGNMENT" );
            case EXCEPTION_FLT_DENORMAL_OPERAND:
               return QStringLiteral( "EXCEPTION_FLT_DENORMAL_OPERAND" );
            case EXCEPTION_FLT_DIVIDE_BY_ZERO:
               return QStringLiteral( "EXCEPTION_FLT_DIVIDE_BY_ZERO" );
            case EXCEPTION_FLT_INEXACT_RESULT:
               return QStringLiteral( "EXCEPTION_FLT_INEXACT_RESULT" );
            case EXCEPTION_FLT_INVALID_OPERATION:
               return QStringLiteral( "EXCEPTION_FLT_INVALID_OPERATION" );
            case EXCEPTION_FLT_OVERFLOW:
               return QStringLiteral( "EXCEPTION_FLT_OVERFLOW" );
            case EXCEPTION_FLT_STACK_CHECK:
               return QStringLiteral( "EXCEPTION_FLT_STACK_CHECK" );
            case EXCEPTION_FLT_UNDERFLOW:
               return QStringLiteral( "EXCEPTION_FLT_UNDERFLOW" );
            case EXCEPTION_ILLEGAL_INSTRUCTION:
               return QStringLiteral( "EXCEPTION_ILLEGAL_INSTRUCTION" );
            case EXCEPTION_IN_PAGE_ERROR:
               return QStringLiteral( "EXCEPTION_IN_PAGE_ERROR" );
            case EXCEPTION_INT_DIVIDE_BY_ZERO:
               return QStringLiteral( "EXCEPTION_INT_DIVIDE_BY_ZERO" );
            case EXCEPTION_INT_OVERFLOW:
               return QStringLiteral( "EXCEPTION_INT_OVERFLOW" );
            case EXCEPTION_INVALID_DISPOSITION:
               return QStringLiteral( "EXCEPTION_INVALID_DISPOSITION" );
            case EXCEPTION_NONCONTINUABLE_EXCEPTION:
               return QStringLiteral( "EXCEPTION_NONCONTINUABLE_EXCEPTION" );
            case EXCEPTION_PRIV_INSTRUCTION:
               return QStringLiteral( "EXCEPTION_PRIV_INSTRUCTION" );
            case EXCEPTION_SINGLE_STEP:
               return QStringLiteral( "EXCEPTION_SINGLE_STEP" );
            case EXCEPTION_STACK_OVERFLOW:
               return QStringLiteral( "EXCEPTION_STACK_OVERFLOW" );
            default:
               return QStringLiteral( "Unrecognized Exception" );
         }
      } ( inExceptionInfo->ExceptionRecord->ExceptionCode );

      // If this is a stack overflow then we can't walk the stack, so just show
      // where the error happened
      QStringList frameInfoList;

      if ( inExceptionInfo->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW )
      {
          DWORD64 offset;
#ifdef _M_IX86
          offset = inExceptionInfo->ContextRecord->Eip;
#elif _M_X64
          offset = inExceptionInfo->ContextRecord->Rip;
#elif _M_IA64
          offset = inExceptionInfo->ContextRecord->StIIP;
#endif
         frameInfoList += _addr2line( sProgramName, (void*)offset );
      }
      else
      {
         frameInfoList += _stackTrace( inExceptionInfo->ContextRecord );
      }

      _writeLog( cExceptionType, frameInfoList );

      return EXCEPTION_EXECUTE_HANDLER;
   }
#else
   constexpr int  MAX_STACK_FRAMES = 64;
   static void    *sStackTraces[MAX_STACK_FRAMES];
   static uint8_t sAlternateStack[SIGSTKSZ];

   QStringList  _stackTrace()
   {
      int   traceSize = backtrace( sStackTraces, MAX_STACK_FRAMES );
      char  **messages = backtrace_symbols( sStackTraces, traceSize );

      // skip the first 2 stack frames (this function and our handler) and skip the last frame (always junk)
      QStringList frameList;
      int         frameNumber = 0;

      frameList.reserve( traceSize );

      for ( int i = 2; i < (traceSize - 1); ++i )
      {
         QString  message( messages[i] );

         // match the mangled name if possible and replace with file & line number
         QRegularExpressionMatch match = sSymbolMatching.match( message );

         const QString  cSymbol( match.captured( 1 ) );

         if ( !cSymbol.isNull() )
         {
            QString  locationStr = _addr2line( sProgramName, sStackTraces [i] );

            if ( !locationStr.isEmpty() )
            {
               int   matchStart = match.capturedStart( 1 );

               message.replace( matchStart, message.length() - matchStart, locationStr );
            }
         }

         frameList += message;

         ++frameNumber;
      }

      if ( messages != nullptr )
      {
         free( messages );
      }

      return frameList;
   }

   // prtotype to prevent warning about not returning
   void _posixSignalHandler( int inSig, siginfo_t *inSigInfo, void *inContext ) __attribute__ ((noreturn));
   void _posixSignalHandler( int inSig, siginfo_t *inSigInfo, void *inContext )
   {
      Q_UNUSED( inContext );

      const QString  cSignalType = [] ( int sig, int inSignalCode ) {
         switch( sig )
         {
            case SIGSEGV:
               return QStringLiteral( "Caught SIGSEGV: Segmentation Fault" );
            case SIGINT:
               return QStringLiteral( "Caught SIGINT: Interactive attention signal, (usually ctrl+c)" );
            case SIGFPE:
               switch( inSignalCode )
               {
                  case FPE_INTDIV:
                     return QStringLiteral( "Caught SIGFPE: (integer divide by zero)" );
                  case FPE_INTOVF:
                     return QStringLiteral( "Caught SIGFPE: (integer overflow)" );
                  case FPE_FLTDIV:
                     return QStringLiteral( "Caught SIGFPE: (floating-point divide by zero)" );
                  case FPE_FLTOVF:
                     return QStringLiteral( "Caught SIGFPE: (floating-point overflow)" );
                  case FPE_FLTUND:
                     return QStringLiteral( "Caught SIGFPE: (floating-point underflow)" );
                  case FPE_FLTRES:
                     return QStringLiteral( "Caught SIGFPE: (floating-point inexact result)" );
                  case FPE_FLTINV:
                     return QStringLiteral( "Caught SIGFPE: (floating-point invalid operation)" );
                  case FPE_FLTSUB:
                     return QStringLiteral( "Caught SIGFPE: (subscript out of range)" );
                  default:
                     return QStringLiteral( "Caught SIGFPE: Arithmetic Exception" );
               }
            case SIGILL:
               switch( inSignalCode )
               {
                  case ILL_ILLOPC:
                     return QStringLiteral( "Caught SIGILL: (illegal opcode)" );
                  case ILL_ILLOPN:
                     return QStringLiteral( "Caught SIGILL: (illegal operand)" );
                  case ILL_ILLADR:
                     return QStringLiteral( "Caught SIGILL: (illegal addressing mode)" );
                  case ILL_ILLTRP:
                     return QStringLiteral( "Caught SIGILL: (illegal trap)" );
                  case ILL_PRVOPC:
                     return QStringLiteral( "Caught SIGILL: (privileged opcode)" );
                  case ILL_PRVREG:
                     return QStringLiteral( "Caught SIGILL: (privileged register)" );
                  case ILL_COPROC:
                     return QStringLiteral( "Caught SIGILL: (coprocessor error)" );
                  case ILL_BADSTK:
                     return QStringLiteral( "Caught SIGILL: (internal stack error)" );
                  default:
                     return QStringLiteral( "Caught SIGILL: Illegal Instruction" );
               }
            case SIGTERM:
               return QStringLiteral( "Caught SIGTERM: a termination request was sent to the program" );
            case SIGABRT:
               return QStringLiteral( "Caught SIGABRT: usually caused by an abort() or assert()" );
         }

         return QStringLiteral( "Unrecognized Signal" );
      } ( inSig, inSigInfo->si_code );

      const QStringList cFrameInfoList = _stackTrace();

      _writeLog( cSignalType, cFrameInfoList );

      _Exit(1);
   }

   void _posixSetupSignalHandler()
   {
      // setup alternate stack
      stack_t ss{ static_cast<void*>(sAlternateStack), SIGSTKSZ, 0 };

      if ( sigaltstack( &ss, nullptr ) != 0 )
      {
         err( 1, "sigaltstack" );
      }

      // register our signal handlers
      struct sigaction sigAction;

      sigAction.sa_sigaction = _posixSignalHandler;

      sigemptyset( &sigAction.sa_mask );

#ifdef __APPLE__
      // backtrace() doesn't work on macOS when we use an alternate stack
      sigAction.sa_flags = SA_SIGINFO;
#else
      sigAction.sa_flags = SA_SIGINFO | SA_ONSTACK;
#endif

      if ( sigaction( SIGSEGV, &sigAction, nullptr ) != 0 ) { err( 1, "sigaction" ); }
      if ( sigaction( SIGFPE,  &sigAction, nullptr ) != 0 ) { err( 1, "sigaction" ); }
      if ( sigaction( SIGINT,  &sigAction, nullptr ) != 0 ) { err( 1, "sigaction" ); }
      if ( sigaction( SIGILL,  &sigAction, nullptr ) != 0 ) { err( 1, "sigaction" ); }
      if ( sigaction( SIGTERM, &sigAction, nullptr ) != 0 ) { err( 1, "sigaction" ); }
      if ( sigaction( SIGABRT, &sigAction, nullptr ) != 0 ) { err( 1, "sigaction" ); }
   }
#endif

   void  setSignalHandler( const QString &inCrashReportDirPath, logWrittenCallback inLogWrittenCallback )
   {
      sProgramName = QCoreApplication::arguments().at( 0 );

      sCrashReportDirPath = inCrashReportDirPath;

      if ( sCrashReportDirPath.isEmpty() )
      {
         sCrashReportDirPath = QStringLiteral( "%1/%2 Crash Logs" ).arg( QStandardPaths::writableLocation( QStandardPaths::DesktopLocation ),
                                                                         QCoreApplication::applicationName() );
      }

      sSymbolMatching.optimize();

      sLogWrittenCallback = inLogWrittenCallback;

      if ( sProcess == nullptr )
      {
         sProcess = new QProcess;

         sProcess->setProcessChannelMode( QProcess::MergedChannels );
      }

#ifdef Q_OS_WIN
      SetUnhandledExceptionFilter( _winExceptionHandler );
#else
      _posixSetupSignalHandler();
#endif
   }
}
