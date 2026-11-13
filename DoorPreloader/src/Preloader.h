#pragma once

// Watches for doors near the player and loads the models of the place behind them ahead of time,
// so the game finds them already in memory when the door is used.
namespace Preloader
{
	void Start();  // main thread, after data has loaded
}
