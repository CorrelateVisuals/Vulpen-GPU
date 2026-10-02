#pragma once

#include "runtime/Operator.h"

#include <string_view>
#include <vector>

namespace VP {

class Commands;
class ViewLookup;
struct Connection;
struct Deploy;
struct Endpoint;
struct Node;

// The primitive edits, which are the manifest's own words: node add, remove and set,
// connect and disconnect, param set and unset, deploy add and remove. Loading a view
// runs its lines through them, so a typed edit and a loaded one change a view alike.
//
// Each edit throws naming the mistake and leaves the view as it was (A02), so a view
// only ever holds whole nodes, and connections between them that form no cycle.
class Edits final : public CommandHandler {
public:
  // Registers the edits; each changes the view the lookup finds.
  Edits(Commands &commands, ViewLookup &views);

  // One node word, as a manifest line or a word=value argument gives it.
  static void word(Node &node, std::string_view key, std::string_view value);
  // One deploy word: its recipe, or a param on one of the recipe's nodes.
  static void word(Deploy &deploy, std::string_view key, std::string_view value);
  // node.port, or deploy.node.port for a port of a deployed recipe's node.
  static Endpoint endpoint(std::string_view text);
  static void add(View &view, Node node);
  static void deploy(View &view, Deploy deploy);
  static void connect(View &view, Connection connection);

private:
  void command(Call &call) override;

  const Commands &_port; // which says where each edit comes from
  ViewLookup &_views;
  std::vector<Command> _commands; // one per edit, in the order they register
};

} // namespace VP
