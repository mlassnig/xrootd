//////////////////////////////////////////////////////////////////////////
//                                                                      //
// XrdClientDebug                                                       //
//                                                                      //
// Author: Fabrizio Furano (INFN Padova, 2004)                          //
// Adapted from TXNetFile (root.cern.ch) originally done by             //
//  Alvise Dorigo, Fabrizio Furano                                      //
//          INFN Padova, 2003                                           //
//                                                                      //
// Singleton used to handle the debug level and the log output          //
//                                                                      //
//////////////////////////////////////////////////////////////////////////

//       $Id$

#ifndef XRC_DEBUG_H
#define XRC_DEBUG_H

#include <iostream>
#include <sstream>
#include "XrdClient/XrdClientConst.hh"
#include "XrdOuc/XrdOucPthread.hh"
#include "XrdClient/XrdClientEnv.hh"
#include "XrdOuc/XrdOucLogger.hh"
#include "XrdOuc/XrdOucError.hh"

using namespace std;



#define DebugLevel() XrdClientDebug::Instance()->GetDebugLevel()
#define DebugSetLevel(l) XrdClientDebug::Instance()->SetLevel(l)

#define Info(lvl, where, what) { \
XrdClientDebug::Instance()->Lock();\
if (XrdClientDebug::Instance()->GetDebugLevel() >= lvl) {\
ostringstream outs;\
outs << where << " " << what; \
XrdClientDebug::Instance()->TraceStream((short)lvl, outs);\
}\
XrdClientDebug::Instance()->Unlock();\
}
                               
#define Error(where, what) { \
ostringstream outs;\
outs << where << " " << what; \
XrdClientDebug::Instance()->TraceStream((short)XrdClientDebug::kNODEBUG, outs);\
}


class XrdClientDebug {
 private:
   short                       fDbgLevel;

   XrdOucLogger                *fOucLog;
   XrdOucError                 *fOucErr;

   static XrdClientDebug       *fgInstance;

   XrdOucRecMutex                 fMutex;

 protected:
   XrdClientDebug();
   ~XrdClientDebug();

 public:

   enum {
      kNODEBUG   = 0,
      kUSERDEBUG = 1,
      kHIDEBUG   = 2,
      kDUMPDEBUG = 3
   };

   short           GetDebugLevel() {
       XrdOucMutexHelper m(fMutex);
       return fDbgLevel;
       }

   static XrdClientDebug *Instance();

   inline void SetLevel(int l) {
      XrdOucMutexHelper m(fMutex);
      fDbgLevel = l;
   }

   inline void TraceStream(short DbgLvl, ostringstream &s) {
      XrdOucMutexHelper m(fMutex);

      if (DbgLvl <= GetDebugLevel())
	 fOucErr->Emsg("", s.str().c_str() );

      s.str("");
   }

   //   ostringstream outs;  // Declare an output string stream.

   inline void TraceString(short DbgLvl, char * s) {
      XrdOucMutexHelper m(fMutex);
      if (DbgLvl <= GetDebugLevel())
	 fOucErr->Emsg("", s);
   }

   inline void Lock() { fMutex.Lock(); }
   inline void Unlock() { fMutex.UnLock(); }

};

#endif
