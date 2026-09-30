#pragma once

namespace VP {

// The primitive edits, which are the manifest's own words: node add, remove and set,
// connect and disconnect, param set and unset, deploy add and remove. Loading a view
// runs its lines through them, so a typed edit and a loaded one change a view alike.
class Edits {};

} // namespace VP
