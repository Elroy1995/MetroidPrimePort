#ifndef _CDVDREQUESTMANAGER
#define _CDVDREQUESTMANAGER

class CDvdRequestSys {
public:
  CDvdRequestSys() {
    if (mManagerInstalled != true) {
      mManagerInstalled = true;
    }
  }
  ~CDvdRequestSys() {
    if (mManagerInstalled == true) {
      mManagerInstalled = false;
    }
  }

  static bool mManagerInstalled;
};


#endif // _CDVDREQUESTMANAGER
