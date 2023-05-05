#pragma once
#include <openxr/openxr.h>

#include "IActionMapManager.h"

class OpenXRInput
{
public:
	void Init(XrInstance instance, XrSession session, XrSpace space);
	void Shutdown();

	void Update();

	Matrix34 GetControllerTransform(int hand);

private:
	struct BooleanAction
	{
		XrAction handle = nullptr;
		ActionId* onPress = nullptr;
		ActionId* onLongPress = nullptr;
		bool sendRelease = true;
		bool longPressActive = false;
		float timePressed = -1;
	};

	XrInstance m_instance = nullptr;
	XrSession m_session = nullptr;
	XrSpace m_trackingSpace = nullptr;

	XrActionSet m_ingameSet = nullptr;
	XrAction m_controller[2] = {};
	XrAction m_moveX = nullptr;
	XrAction m_moveY = nullptr;
	XrAction m_rotateYaw = nullptr;
	XrAction m_rotatePitch = nullptr;
	XrAction m_jumpCrouch = nullptr;
	BooleanAction m_primaryFire;
	BooleanAction m_sprint;
	BooleanAction m_reload;
	BooleanAction m_menu;
	BooleanAction m_suitMenu;
	BooleanAction m_binoculars;
	BooleanAction m_nextWeapon;
	BooleanAction m_use;
	BooleanAction m_nightvision;
	BooleanAction m_melee;
	XrSpace m_gripSpace[2] = {};
	bool m_wasJumpActive = false;
	bool m_wasCrouchActive = false;

	void CreateInputActions();
	void SuggestBindings();
	void CreateBooleanAction(XrActionSet actionSet, BooleanAction& action, const char* name, const char* description, ActionId* onPress, ActionId* onLongPress = nullptr, bool sendRelease = true);
	void UpdatePlayerMovement();
	void UpdateBooleanAction(BooleanAction& action);
};
