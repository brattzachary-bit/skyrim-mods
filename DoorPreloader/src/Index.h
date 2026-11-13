#pragma once

// What every cell contains, worked out from the plugin files themselves (the game only knows a cell's
// contents once it has loaded it). Built in the background after the game data has loaded and kept
// on disk until the load order changes.
namespace Index
{
	void BuildAsync();  // main thread, after data has loaded
	bool Ready();

	// Base objects placed in an interior cell; nullptr when unknown.
	const std::vector<RE::FormID>* Interior(RE::FormID a_cell);

	// Base objects placed in the exterior cells within a_radius cells of (a_x, a_y); appended to a_out.
	void Exterior(RE::FormID a_world, std::int32_t a_x, std::int32_t a_y, std::int32_t a_radius, std::vector<RE::FormID>& a_out);
}
