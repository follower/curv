// Copyright Doug Moen 2016-2017.
// Distributed under The MIT License.
// See accompanying file LICENSE.md or https://opensource.org/licenses/MIT

#ifndef CURV_PARSE_H
#define CURV_PARSE_H

#include <curv/phrase.h>
#include <curv/scanner.h>

namespace curv {

Shared<Program_Phrase> parse_program(Scanner&);

} // namespace curv
#endif // header guard
