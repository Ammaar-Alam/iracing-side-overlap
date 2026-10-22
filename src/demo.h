// Scripted traffic run through the real estimator, for layout and development
#pragma once
#include "estimator.h"
#include "frame.h"

// Session geometry matching the demo frames
SessionGeometry demoGeometry();

// Fills a frame for scenario time t, returns false while the script simulates a dead feed
bool demoFrame(double t, Frame& f);
