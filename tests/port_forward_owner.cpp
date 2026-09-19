#include "rstl/rc_ptr.hpp"

class ForwardDeleted;
// Last-owner destruction must work in a TU with only a forward declaration.
void DropForwardOwner(rstl::rc_ptr<ForwardDeleted>& owner) { owner.reset(); }
