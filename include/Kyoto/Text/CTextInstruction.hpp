#ifndef _CTEXTINSTRUCTION
#define _CTEXTINSTRUCTION

#include "Kyoto/Text/CInstruction.hpp"
#ifdef TARGET_PC
#include <string>
#endif

class CTextInstruction : public CInstruction {
public:
  static CTextInstruction* Create(const wchar_t* str, const int len);
  CTextInstruction(const wchar_t* str, int length);
  void Invoke(CFontRenderState& state, CTextRenderBuffer* buffer) const override;

private:
  const wchar_t* Text() const {
#ifdef TARGET_PC
    return mString.data();
#else
    return mString;
#endif
  }
  int mLength;
#ifdef TARGET_PC
  std::wstring mString;
#else
  wchar_t mString[1];
#endif
};

#endif // _CTEXTINSTRUCTION
