#pragma once

// The first window of the program, before anything touches the game. The version is checked against GitHub when it
// opens: Start stays locked while a newer version is out, the version is marked unsafe or GitHub cannot be reached.
// Start finds the game (its offsets, -insecure, the config), the list of the items & downloads the pictures of the
// items not on the disk yet. The features & the overlay start once all of it is done
class Loader {
public:
    // Shows it until Start is done (true) or it is closed (false)
    static bool Run();
};
