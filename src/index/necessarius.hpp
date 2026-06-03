#pragma once

#include "index/artificial_heaven.hpp"
#include "index/index_librorum_prohibitorum.hpp"
#include "index/tree_diagram.hpp"

namespace index {

[[noreturn]] void enter_necessarius(const ArtificialHeaven &heaven,
                                    IndexLibrorumProhibitorum &grimoires,
                                    TreeDiagram &tree);

} // namespace index
