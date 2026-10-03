#pragma once

// Builds maps/<name>.tri from the map files of the game, same as scripts/export_map_collision.py
namespace MapExport {
    // Folder with the map vpks of the running game, empty when it could not be found
    std::filesystem::path FindMapsDir();

    // Reads the world physics of the map & writes its collision to out. False when the map has none
    // progress gets 0..1 while it goes
    bool Export(const std::string& name, const std::filesystem::path& out, const std::function<void(float)>& progress = {});
}
