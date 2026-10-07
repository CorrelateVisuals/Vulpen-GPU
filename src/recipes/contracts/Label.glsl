// A run of text in one color and the room it has: the range of a list of characters
// that it shows. Every part that shows text writes Labels and only glyphs lays them out,
// so text is shaped in one place.
struct Label {
  ivec2 offset; // where its first character's cell starts, in pixels
  uvec2 extent; // the room it has; a character past it is not drawn
  uint role;    // the palette color of its text, as contracts/Palette.h names the roles
  uint first;   // its first character in the list
  uint count;   // how many characters it shows
};

// One character of the list: its code, and the label it belongs to. Its writer keeps the
// label among the Labels it wrote, as a shader cannot know how many there are (GLSL02).
struct Character {
  uint code;  // Unicode; the font draws printable ASCII
  uint label; // its label in the list of labels
};

layout(buffer_reference, std430) readonly buffer Labels {
  Label at[];
};

layout(buffer_reference, std430) readonly buffer Characters {
  Character at[];
};
