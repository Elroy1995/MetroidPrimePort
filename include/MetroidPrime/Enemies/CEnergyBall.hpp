// Port: this used to be a second, padded definition of CEnergyBall under the
// same include guard. Its size only matched the real class on the 32-bit
// target, so ScriptLoader allocated the padded size and the real constructor
// (ScriptObjects/CEnergyBall.cpp) wrote past the end of the block when Impact
// Crater loaded its energy balls. Keep one definition.
#include "MetroidPrime/ScriptObjects/CEnergyBall.hpp"
