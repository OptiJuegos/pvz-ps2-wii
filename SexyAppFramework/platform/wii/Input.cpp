#ifdef WII_PLATFORM

#include <cmath>
#include <gccore.h>
#include <wiiuse/wpad.h>

#include "SexyAppBase.h"
#include "misc/KeyCodes.h"
#include "widget/WidgetManager.h"
#include "widget/VirtualKeyboard.h"
#include "../../../LawnApp.h"
#include "../../../Lawn/Board.h"
#include "../../../Lawn/Widget/SeedChooserScreen.h"

using namespace Sexy;

namespace
{
float sCursorX = 320.0f;
float sCursorY = 240.0f;
float sKeyboardAnchorX = 320.0f;
float sKeyboardAnchorY = 240.0f;
float sCursorRotation = 0.0f;
u32 sPrevWpad = 0;
u32 sPrevPad = 0;
bool sAUsesKeyboardSelection = false;
const float kIrFocusReleaseDistance = 12.0f;

void UpdateCursorRotation(float theRoll)
{
	float aDelta = theRoll - sCursorRotation;
	while (aDelta > 180.0f) aDelta -= 360.0f;
	while (aDelta < -180.0f) aDelta += 360.0f;
	sCursorRotation += aDelta * 0.2f;
	if (sCursorRotation > 180.0f) sCursorRotation -= 360.0f;
	if (sCursorRotation < -180.0f) sCursorRotation += 360.0f;
}

void ClearKeyboardSeedSelection()
{
	if (gLawnApp == NULL)
		return;
	if (gLawnApp->mSeedChooserScreen != NULL)
		gLawnApp->mSeedChooserScreen->ClearKeyboardSelection();
	if (gLawnApp->mBoard != NULL)
		gLawnApp->mBoard->ClearKeyboardSeedSelection();
}

void SendPointer(SexyAppBase* app)
{
	int x = static_cast<int>(sCursorX);
	int y = static_cast<int>(sCursorY);
	app->mWidgetManager->RemapMouse(x, y);
	app->mLastUserInputTick = app->mLastTimerTime;
	app->mWidgetManager->MouseMove(x, y);
}

void SendButton(SexyAppBase* app, bool down, int button)
{
	int x = static_cast<int>(sCursorX);
	int y = static_cast<int>(sCursorY);
	app->mWidgetManager->RemapMouse(x, y);
	app->mLastUserInputTick = app->mLastTimerTime;
	app->mWidgetManager->MouseMove(x, y);
	if (down)
		app->mWidgetManager->MouseDown(x, y, button);
	else
		app->mWidgetManager->MouseUp(x, y, button);
}

void SendKey(SexyAppBase* app, KeyCode key, bool down)
{
	app->mLastUserInputTick = app->mLastTimerTime;
	if (down) app->mWidgetManager->KeyDown(key);
	else app->mWidgetManager->KeyUp(key);
}
}

// Published for the renderer: the Wii has no OS cursor, so GLInterface::Redraw
// overlays the software cursor here. Screen coordinates (the WPAD_SetVRes
// space), NOT game coordinates -- the cursor is drawn after the frame is
// composed, in the same space the pointer is reported in.
void WiiGetCursorState(float& theX, float& theY, float& theRotation)
{
	theX = sCursorX;
	theY = sCursorY;
	theRotation = sCursorRotation;
}

void SexyAppBase::InitInput()
{
	WPAD_SetDataFormat(WPAD_CHAN_0, WPAD_FMT_BTNS_ACC_IR);
	WPAD_SetVRes(WPAD_CHAN_0, 640, 480);
	mMouseIn = true;
}

bool SexyAppBase::StartTextInput(std::string&)
{
	return VirtualKeyboardShow(this);
}

void SexyAppBase::StopTextInput()
{
	VirtualKeyboardHide(this);
}

bool SexyAppBase::ProcessDeferredMessages(bool)
{
	WPAD_ScanPads();
	PAD_ScanPads();

	WPADData* wd = WPAD_Data(WPAD_CHAN_0);
	if (wd != NULL)
		UpdateCursorRotation(wd->orient.roll);
	else
		UpdateCursorRotation(0.0f);

	if (wd && wd->ir.valid)
	{
		// Wii Remote pointing at the sensor bar: absolute positioning, already
		// in the 640x480 space InitInput asked WPAD for.
		if (std::fabs(wd->ir.x - sKeyboardAnchorX) >= kIrFocusReleaseDistance ||
			std::fabs(wd->ir.y - sKeyboardAnchorY) >= kIrFocusReleaseDistance)
		{
			ClearKeyboardSeedSelection();
			sKeyboardAnchorX = wd->ir.x;
			sKeyboardAnchorY = wd->ir.y;
		}
		sCursorX = wd->ir.x;
		sCursorY = wd->ir.y;
	}
	else
	{
		// GameCube stick (or a Remote aimed off-screen): relative movement.
		// The stick rests near zero but never exactly at it, so without a
		// deadzone the cursor drifts across the screen on its own.
		const float aSpeed = 8.0f;
		const int aDeadZone = 12;			// of the stick's +-128 range
		int aStickX = PAD_StickX(0);
		int aStickY = PAD_StickY(0);
		if (aStickX > -aDeadZone && aStickX < aDeadZone) aStickX = 0;
		if (aStickY > -aDeadZone && aStickY < aDeadZone) aStickY = 0;
		if (aStickX != 0 || aStickY != 0)
			ClearKeyboardSeedSelection();
		sCursorX += aStickX * (aSpeed / 128.0f);
		sCursorY -= aStickY * (aSpeed / 128.0f);	// stick Y is up-positive
	}

	if (sCursorX < 0.0f) sCursorX = 0.0f;
	if (sCursorX > 639.0f) sCursorX = 639.0f;
	if (sCursorY < 0.0f) sCursorY = 0.0f;
	if (sCursorY > 479.0f) sCursorY = 479.0f;
	SendPointer(this);

	u32 wpad = WPAD_ButtonsHeld(WPAD_CHAN_0);
	// When the Wii Remote IR pointer is active (or any Wiimote button is pressed),
	// ignore GameCube port 1 buttons so Dolphin's default keyboard mapping
	// (where Wiimote A=Z, B=X and GCPad A=X, B=Z are inverted on the same keys)
	// does not fire a simultaneous right-click cancel (PAD_BUTTON_B) when pressing A.
	const bool aWiimoteActive = (wd != NULL && wd->ir.valid) || wpad != 0 || sPrevWpad != 0;
	u32 pad = aWiimoteActive ? 0 : PAD_ButtonsHeld(0);
	u32 wDown = wpad & ~sPrevWpad;
	u32 wUp = sPrevWpad & ~wpad;
	u32 pDown = pad & ~sPrevPad;
	u32 pUp = sPrevPad & ~pad;

	const bool aChooserSeedSelected = gLawnApp != NULL &&
		gLawnApp->mSeedChooserScreen != NULL &&
		gLawnApp->mSeedChooserScreen->HasKeyboardSelection();
	const bool aBoardSeedSelected = gLawnApp != NULL && gLawnApp->mBoard != NULL &&
		gLawnApp->mBoard->HasKeyboardSeedSelection();
	const bool aKeyboardSeedSelected = aChooserSeedSelected || aBoardSeedSelected;
	const bool anAIsHeld = (wpad & WPAD_BUTTON_A) != 0 || (pad & PAD_BUTTON_A) != 0;
	const bool anAWasHeld = (sPrevWpad & WPAD_BUTTON_A) != 0 ||
		(sPrevPad & PAD_BUTTON_A) != 0;

	// On-screen keyboard: hand it the D-Pad, A and B while the player is
	// driving it that way. It returns false in pointer mode, where the IR
	// aims at keys and everything below behaves as it always has.
	unsigned int aNav = 0;
	if ((wpad & WPAD_BUTTON_LEFT) || (pad & PAD_BUTTON_LEFT))	aNav |= VKNAV_LEFT;
	if ((wpad & WPAD_BUTTON_RIGHT) || (pad & PAD_BUTTON_RIGHT))	aNav |= VKNAV_RIGHT;
	if ((wpad & WPAD_BUTTON_UP) || (pad & PAD_BUTTON_UP))		aNav |= VKNAV_UP;
	if ((wpad & WPAD_BUTTON_DOWN) || (pad & PAD_BUTTON_DOWN))	aNav |= VKNAV_DOWN;
	if (anAIsHeld)												aNav |= VKNAV_ACCEPT;
	if ((wpad & WPAD_BUTTON_B) || (pad & PAD_BUTTON_B))			aNav |= VKNAV_BACK;
	const bool aKeyboardNav = VirtualKeyboardFeedNav(aNav);

	if (aKeyboardNav)
	{
		// The keyboard owns these this frame. Still track the edge state below
		// so releasing a button after the keyboard closes is not seen as a
		// fresh press.
		sPrevWpad = wpad;
		sPrevPad = pad;
		return false;
	}

	if (anAIsHeld && !anAWasHeld)
	{
		sAUsesKeyboardSelection = aKeyboardSeedSelected;
		if (sAUsesKeyboardSelection) SendKey(this, KEYCODE_RETURN, true);
		else SendButton(this, true, 1);
	}
	if (!anAIsHeld && anAWasHeld)
	{
		if (sAUsesKeyboardSelection) SendKey(this, KEYCODE_RETURN, false);
		else SendButton(this, false, 1);
		sAUsesKeyboardSelection = false;
	}
	if ((wDown & WPAD_BUTTON_B) || (pDown & PAD_BUTTON_B)) SendButton(this, true, -1);
	if ((wUp & WPAD_BUTTON_B) || (pUp & PAD_BUTTON_B)) SendButton(this, false, -1);

	struct DirectionMapping { u32 wpadButton; u32 padButton; KeyCode key; };
	static const DirectionMapping kDirections[] = {
		{ WPAD_BUTTON_UP, PAD_BUTTON_UP, KEYCODE_UP }, { WPAD_BUTTON_DOWN, PAD_BUTTON_DOWN, KEYCODE_DOWN },
		{ WPAD_BUTTON_LEFT, PAD_BUTTON_LEFT, KEYCODE_LEFT }, { WPAD_BUTTON_RIGHT, PAD_BUTTON_RIGHT, KEYCODE_RIGHT }
	};
	for (unsigned i = 0; i < sizeof(kDirections) / sizeof(kDirections[0]); i++)
	{
		const DirectionMapping& aDirection = kDirections[i];
		const bool aDirectionIsHeld = (wpad & aDirection.wpadButton) != 0 ||
			(pad & aDirection.padButton) != 0;
		const bool aDirectionWasHeld = (sPrevWpad & aDirection.wpadButton) != 0 ||
			(sPrevPad & aDirection.padButton) != 0;
		if (aDirectionIsHeld && !aDirectionWasHeld)
		{
			sKeyboardAnchorX = sCursorX;
			sKeyboardAnchorY = sCursorY;
			SendKey(this, aDirection.key, true);
		}
		if (!aDirectionIsHeld && aDirectionWasHeld)
			SendKey(this, aDirection.key, false);
	}

	const bool aBoardIsActive = gLawnApp != NULL && gLawnApp->mBoard != NULL &&
		gLawnApp->mGameScene == GameScenes::SCENE_PLAYING &&
		!gLawnApp->mBoard->mPaused && gLawnApp->GetDialogCount() == 0;
	if ((wDown & WPAD_BUTTON_PLUS) || (pDown & PAD_BUTTON_START))
		SendKey(this, aBoardIsActive ? KEYCODE_ESCAPE : KEYCODE_RETURN, true);
	if ((wUp & WPAD_BUTTON_PLUS) || (pUp & PAD_BUTTON_START))
	{
		SendKey(this, KEYCODE_ESCAPE, false);
		SendKey(this, KEYCODE_RETURN, false);
	}
	if (wDown & WPAD_BUTTON_HOME)
	{
		mShutdown = true;
		sPrevWpad = wpad;
		sPrevPad = pad;
		return false;
	}

	sPrevWpad = wpad;
	sPrevPad = pad;

	// FALSE MEANS "THE QUEUE IS DRAINED", NOT "QUIT".
	//
	// UpdateAppStep only leaves UPDATESTATE_MESSAGES when this returns false:
	//
	//     if (!ProcessDeferredMessages(true))
	//         mUpdateAppState = UPDATESTATE_PROCESS_1;
	//
	// and UpdateApp() spins on UpdateAppStep until an update actually happens.
	// Returning !mShutdown (i.e. true while running) therefore said "there are
	// still messages pending" forever: the state never advanced, Process() was
	// never called, nothing updated or drew, and the main loop burned ~330k
	// iterations a second doing nothing. That was the boot hang at the loading
	// bar -- the loading thread only starts once TitleScreen has been drawn.
	//
	// Compare the two working backends: the PC one returns
	// SDL_HasEvents(...) -- true only while more events remain -- and the PS2
	// one returns false, because like this one it drains the pads completely on
	// every call. There is nothing left queued here either, so: false.
	return false;
}

#endif
