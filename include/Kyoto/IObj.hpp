#ifndef _IOBJ
#define _IOBJ

#include "types.h"

#include "Kyoto/Alloc/CMemory.hpp"
#include "Kyoto/SObjectTag.hpp"
#include "rstl/auto_ptr.hpp"

extern const SObjectTag gkInvalidObjectTag;

class IObj {
public:
  virtual ~IObj() = 0;
};

inline IObj::~IObj() {}

class CObjOwnerDerivedFromIObjUntyped : public IObj {
public:
  ~CObjOwnerDerivedFromIObjUntyped() = 0;
  template < typename T >
  CObjOwnerDerivedFromIObjUntyped(T* obj) : m_objPtr(obj) {
#ifdef TARGET_PC
    mDelete = &Destroy<T>;
#endif
  }
  template < typename T >
  CObjOwnerDerivedFromIObjUntyped(const rstl::auto_ptr< T >& obj) : m_objPtr(obj.release()) {
#ifdef TARGET_PC
    mDelete = &Destroy<T>;
#endif
  }

  void* GetContents() const { return m_objPtr; }

protected:
  void* m_objPtr;
#ifdef TARGET_PC
  template < typename T > static void Destroy(void* ptr) {
    static_assert(sizeof(T) > 0, "resource ownership requires a complete type");
    delete static_cast<T*>(ptr);
  }
  void (*mDelete)(void*) = nullptr;
#endif
};

inline CObjOwnerDerivedFromIObjUntyped::~CObjOwnerDerivedFromIObjUntyped() {
#ifdef TARGET_PC
  if (mDelete != nullptr) mDelete(m_objPtr);
#endif
}

template < typename T >
class TObjOwnerDerivedFromIObj : public CObjOwnerDerivedFromIObjUntyped {
public:
  ~TObjOwnerDerivedFromIObj() {
#ifndef TARGET_PC
    if (Owned()) {
      delete Owned();
    }
#endif
  }
  T* Owned() { return static_cast< T* >(m_objPtr); }

  static rstl::auto_ptr< TObjOwnerDerivedFromIObj< T > > GetNewDerivedObject(T* obj) {
    return rs_new TObjOwnerDerivedFromIObj< T >(obj);
  }
  static rstl::auto_ptr< TObjOwnerDerivedFromIObj< T > >
  GetNewDerivedObject(const rstl::auto_ptr< T >& obj) {
    return rs_new TObjOwnerDerivedFromIObj< T >(obj);
  }

private:
  TObjOwnerDerivedFromIObj(T* obj) : CObjOwnerDerivedFromIObjUntyped(obj) {}
  TObjOwnerDerivedFromIObj(const rstl::auto_ptr< T >& obj) : CObjOwnerDerivedFromIObjUntyped(obj) {}
};

#endif // _IOBJ
