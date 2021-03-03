#include "StdAfx.h"
#include "Player.h"
#include "SpawnPoint.h"
#include "GamePlugin.h"

#include <CryRenderer/IRenderAuxGeom.h>
#include <CrySchematyc/Env/Elements/EnvComponent.h>
#include <CryCore/StaticInstanceList.h>
#include <CryNetwork/Rmi.h>

#define MOUSE_DELTA_TRESHOLD 0.0001f

namespace
{
	static void RegisterPlayerComponent(Schematyc::IEnvRegistrar& registrar)
	{
		Schematyc::CEnvRegistrationScope scope = registrar.Scope(IEntity::GetEntityScopeGUID());
		{
			Schematyc::CEnvRegistrationScope componentScope = scope.Register(SCHEMATYC_MAKE_ENV_COMPONENT(CPlayerComponent));
		}
	}

	CRY_STATIC_AUTO_REGISTER_FUNCTION(&RegisterPlayerComponent);
}

void CPlayerComponent::Initialize()
{
	// The character controller is responsible for maintaining player physics
	m_pCharacterController = m_pEntity->GetOrCreateComponent<Cry::DefaultComponents::CCharacterControllerComponent>();
	// Offset the default character controller up by one unit
	m_pCharacterController->SetTransformMatrix(Matrix34::Create(Vec3(1.f), IDENTITY, Vec3(0, 0, 1.f)));

	// Mark the entity to be replicated over the network
	m_pEntity->GetNetEntity()->BindToNetwork();
	
	// Register the RemoteReviveOnClient function as a Remote Method Invocation (RMI) that can be executed by the server on clients
	SRmi<RMI_WRAP(&CPlayerComponent::RemoteReviveOnClient)>::Register(this, eRAT_NoAttach, false, eNRT_ReliableOrdered);
	pe_player_dynamics params;
	//params.gravity = ZERO;
	params.kInertia = 0;
	params.gravity = Vec3(0, 0, -20);
	params.kInertiaAccel = 0;
	params.kAirControl = 3;
	GetEntity()->GetPhysics()->SetParams(&params);
	// Process mouse input to update look orientation.
}

void CPlayerComponent::InitializeLocalPlayer()
{
	// Create the camera component, will automatically update the viewport every frame
	m_pCameraComponent = m_pEntity->GetOrCreateComponent<Cry::DefaultComponents::CCameraComponent>();

	// Create the audio listener component.
	m_pAudioListenerComponent = m_pEntity->GetOrCreateComponent<Cry::Audio::DefaultComponents::CListenerComponent>();

	// Get the input component, wraps access to action mapping so we can easily get callbacks when inputs are triggered
	m_pInputComponent = m_pEntity->GetOrCreateComponent<Cry::DefaultComponents::CInputComponent>();
	
	// Register an action, and the callback that will be sent when it's triggered
	m_pInputComponent->RegisterAction("player", "moveleft", [this](int activationMode, float value) { HandleInputFlagChange(EInputFlag::MoveLeft, (EActionActivationMode)activationMode);  }); 
	// Bind the 'A' key the "moveleft" action
	m_pInputComponent->BindAction("player", "moveleft", eAID_KeyboardMouse, EKeyId::eKI_A);

	m_pInputComponent->RegisterAction("player", "moveright", [this](int activationMode, float value) { HandleInputFlagChange(EInputFlag::MoveRight, (EActionActivationMode)activationMode);  }); 
	m_pInputComponent->BindAction("player", "moveright", eAID_KeyboardMouse, EKeyId::eKI_D);

	m_pInputComponent->RegisterAction("player", "moveforward", [this](int activationMode, float value) { HandleInputFlagChange(EInputFlag::MoveForward, (EActionActivationMode)activationMode);  }); 
	m_pInputComponent->BindAction("player", "moveforward", eAID_KeyboardMouse, EKeyId::eKI_W);

	m_pInputComponent->RegisterAction("player", "moveback", [this](int activationMode, float value) { HandleInputFlagChange(EInputFlag::MoveBack, (EActionActivationMode)activationMode);  }); 
	m_pInputComponent->BindAction("player", "moveback", eAID_KeyboardMouse, EKeyId::eKI_S);

	m_pInputComponent->RegisterAction("player", "mouse_rotateyaw", [this](int activationMode, float value) { m_mouseDeltaRotation.x -= value; });
	m_pInputComponent->BindAction("player", "mouse_rotateyaw", eAID_KeyboardMouse, EKeyId::eKI_MouseX);

	m_pInputComponent->RegisterAction("player", "mouse_rotatepitch", [this](int activationMode, float value) { m_mouseDeltaRotation.y -= value; });
	m_pInputComponent->BindAction("player", "mouse_rotatepitch", eAID_KeyboardMouse, EKeyId::eKI_MouseY);

	m_pInputComponent->RegisterAction("player", "jump", [this](int activationMode, float value) {
		if (activationMode == eAAM_OnPress) {
			wishJump = true;
		}
		else if (activationMode == eAAM_OnRelease) {
			wishJump = false;
		}
		OutputDebugString("Jump pressed");
		}
	);

	m_pInputComponent->BindAction("player", "jump", eAID_KeyboardMouse, EKeyId::eKI_Space);
}


Cry::Entity::EventFlags CPlayerComponent::GetEventMask() const
{
	return
		Cry::Entity::EEvent::BecomeLocalPlayer |
		Cry::Entity::EEvent::Update |
		Cry::Entity::EEvent::Reset;
}
void CPlayerComponent::Update(float frameTime) {
	// Start by updating the movement request we want to send to the character controller
	// This results in the physical representation of the character moving
	m_frametime = frameTime;
	
	
	UpdateLookDirectionRequest(frameTime);

	// Update the animation state of the character
	UpdateLookRotationZ(frameTime);

	if (IsLocalClient())
	{
		// Update the camera component offset
		UpdateCamera(frameTime);
	}

	QueueJump();
	if (m_pCharacterController->IsOnGround()) {
		GroundMove();
		OutputDebugString("\nOn the ground! GroundMoving");
	}
	else if (!m_pCharacterController->IsOnGround()) {
		AirMove();
		OutputDebugString("\nNot on ground! not groundmoving.");
	}

}
void CPlayerComponent::ProcessEvent(const SEntityEvent& event)
{
	switch (event.event)
	{
	case Cry::Entity::EEvent::BecomeLocalPlayer:
	{
		InitializeLocalPlayer();
	}
	break;
	case Cry::Entity::EEvent::Update:
	{
		// Don't update the player if we haven't spawned yet
		if(!m_isAlive)
			return;
		
		const float frameTime = event.fParam[0];
		Update(frameTime);

	}
	break;
	case Cry::Entity::EEvent::Reset:
	{
		// Disable player when leaving game mode.
		m_isAlive = event.nParam[0] != 0;
	}
	break;
	}
}

bool CPlayerComponent::NetSerialize(TSerialize ser, EEntityAspects aspect, uint8 profile, int flags)
{
	if(aspect == InputAspect)
	{
		ser.BeginGroup("PlayerInput");

		const CEnumFlags<EInputFlag> prevInputFlags = m_inputFlags;

		ser.Value("m_inputFlags", m_inputFlags.UnderlyingValue(), 'ui8');

		if (ser.IsReading())
		{
			const CEnumFlags<EInputFlag> changedKeys = prevInputFlags ^ m_inputFlags;

			const CEnumFlags<EInputFlag> pressedKeys = changedKeys & prevInputFlags;
			if (!pressedKeys.IsEmpty())
			{
				HandleInputFlagChange(pressedKeys, eAAM_OnPress);
			}

			const CEnumFlags<EInputFlag> releasedKeys = changedKeys & prevInputFlags;
			if (!releasedKeys.IsEmpty())
			{
				HandleInputFlagChange(pressedKeys, eAAM_OnRelease);
			}
		}

		// Serialize the player look orientation
		ser.Value("m_lookOrientation", m_lookOrientation, 'ori3');

		ser.EndGroup();
	}

	return true;
}


void CPlayerComponent::SetMovementDir()
{

	_cmd.rightMove = 0;
	_cmd.forwardMove = 0;

	if (m_inputFlags & EInputFlag::MoveLeft)
	{
		_cmd.rightMove -= 1;
	}
	if (m_inputFlags & EInputFlag::MoveRight)
	{
		_cmd.rightMove += 1;
	}
	if (m_inputFlags & EInputFlag::MoveForward)
	{
		_cmd.forwardMove += 1;
	}
	if (m_inputFlags & EInputFlag::MoveBack)
	{
		_cmd.forwardMove -= 1;
	}
	CLAMP(_cmd.forwardMove, -1, 1);
	CLAMP(_cmd.rightMove, -1, 1);
	

}

void CPlayerComponent::QueueJump() {
		//wishJump = (m_inputFlags & EInputFlag::Jump) ? true : false;
}

void CPlayerComponent::AirMove() {
	Vec3 wishdir;
	//float wishvel = airAcceleration;
	float accel;

	SetMovementDir();
	wishdir = Vec3(_cmd.rightMove, _cmd.forwardMove, 0);
	wishdir = GetEntity()->GetWorldRotation() * wishdir;

	float wishspeed = wishdir.GetLength();
	wishspeed *= m_moveSpeed;

	wishdir.Normalize();
	moveDirectionNorm = wishdir;

	//Aircontrol
	float wishspeed2 = wishspeed;
	if (playerVelocity.dot(wishdir) < 0) {
		accel = airDecceleration;
	}
	else {
		accel = airAcceleration;
	}
	if (_cmd.forwardMove == 0 && _cmd.rightMove != 0) {
		if (wishspeed > sideStrafeSpeed) {
			wishspeed = sideStrafeSpeed;
		}
		accel = sideStrafeAcceleration;
	}
	Accelerate(wishdir, wishspeed, accel);
	if (airControl > 0) {
		AirControl(wishdir, wishspeed2);
	}
	playerVelocity.z -= gravity * m_frametime;
	pe_action_move moveAction;
	
	moveAction.dir = playerVelocity * m_frametime;
	//GetEntity()->GetPhysics()->Action(&moveAction);
	m_pCharacterController->SetVelocity(playerVelocity * m_frametime);
}

void CPlayerComponent::Accelerate(Vec3 wishdir, float wishspeed, float accel)
{
	float addspeed, accelspeed, currentspeed;

	currentspeed = playerVelocity.dot(wishdir);
	addspeed = wishspeed - currentspeed;
	if (addspeed <= 0)
		return;
	accelspeed = accel * m_frametime * wishspeed;
	if (accelspeed > addspeed)
		accelspeed = addspeed;
	playerVelocity.x += accelspeed * wishdir.x;
	playerVelocity.y += accelspeed * wishdir.y;

}

void CPlayerComponent::AirControl(Vec3 wishdir, float wishspeed)
{
	float zspeed, speed, dot, k;
	if (_cmd.forwardMove == 0 || wishspeed == 0) {
		return;
	}
	zspeed = playerVelocity.z;
	playerVelocity.z = 0;
	speed = playerVelocity.GetLength();
	playerVelocity.Normalize();

	dot = playerVelocity.dot(wishdir);
	k = 32;
	k *= airControl * dot * dot * m_frametime;

	if (dot > 0)
	{
		playerVelocity.x = playerVelocity.x * speed + wishdir.x * k;
		playerVelocity.y = playerVelocity.y * speed + wishdir.y * k;
		playerVelocity.z = playerVelocity.z * speed + wishdir.z * k;
		
		playerVelocity.Normalize();
		moveDirectionNorm = playerVelocity;
	}

	playerVelocity.x *= speed;
	playerVelocity.z = zspeed;
	playerVelocity.y *= speed;
}

void CPlayerComponent::GroundMove() {
	Vec3 wishdir;
	
	if (!wishJump)
		ApplyFriction(1.0f);
	else {

		OutputDebugString("\nI desire to jump\n");
		ApplyFriction(0);
	}
	SetMovementDir();

	wishdir = Vec3(_cmd.rightMove, _cmd.forwardMove, 0);
	wishdir = GetEntity()->GetWorldRotation() * wishdir;
	wishdir.Normalize();
	moveDirectionNorm = wishdir;

	float wishspeed = wishdir.GetLength();
	wishspeed *= m_moveSpeed;

	Accelerate(wishdir, wishspeed, runAcceleration);
	
	playerVelocity.z = 0;
	pe_action_move moveAction;
	moveAction.dir = playerVelocity * m_frametime;
	if (wishJump) {
		pe_action_impulse jumpAction;
		jumpAction.impulse.z = 800;
		GetEntity()->GetPhysics()->Action(&jumpAction);
		wishJump = false;
	}
	//GetEntity()->GetPhysics()->Action(&moveAction);
	m_pCharacterController->SetVelocity(playerVelocity * m_frametime);


}

void CPlayerComponent::ApplyFriction(float t) {
	Vec3 vec = playerVelocity;
	float speed, newspeed, control, drop;
	vec.y = 0.0f;
	speed = vec.GetLength();
	drop = 0.0f;

	if (m_pCharacterController->IsOnGround()) {
		control = speed < runDeacceleration ? runDeacceleration : speed;
		drop = control * friction * m_frametime * t;
	}
	newspeed = speed - drop;
	playerFriction = newspeed;
	if (newspeed < 0) {
		newspeed = 0;
	}
	if (speed > 0) {
		newspeed /= speed;
	}
	playerVelocity.x *= newspeed;
	playerVelocity.y *= newspeed;
}
void CPlayerComponent::UpdateLookDirectionRequest(float frameTime)
{
	const float rotationSpeed = 0.002f;
	const float rotationLimitsMinPitch = -0.84f;
	const float rotationLimitsMaxPitch = 1.5f;
	
	// Update angular velocity metrics
	m_horizontalAngularVelocity = (m_mouseDeltaRotation.x * rotationSpeed) / frameTime;
	m_averagedHorizontalAngularVelocity.Push(m_horizontalAngularVelocity);

	if (m_mouseDeltaRotation.IsEquivalent(ZERO, MOUSE_DELTA_TRESHOLD))
		return;

	// Start with updating look orientation from the latest input
	Ang3 ypr = CCamera::CreateAnglesYPR(Matrix33(m_lookOrientation));

	// Yaw
	ypr.x += m_mouseDeltaRotation.x * rotationSpeed;

	// Pitch
	// TODO: Perform soft clamp here instead of hard wall, should reduce rot speed in this direction when close to limit.
	ypr.y = CLAMP(ypr.y + m_mouseDeltaRotation.y * rotationSpeed, rotationLimitsMinPitch, rotationLimitsMaxPitch);

	// Roll (skip)
	ypr.z = 0;

	m_lookOrientation = Quat(CCamera::CreateOrientationYPR(ypr));

	// Reset the mouse delta accumulator every frame
	m_mouseDeltaRotation = ZERO;
}

void CPlayerComponent::UpdateLookRotationZ(float frameTime) {
	Ang3 ypr = CCamera::CreateAnglesYPR(Matrix33(m_lookOrientation));
	ypr.y = 0;
	ypr.z = 0;
	const Quat correctedOrientation = Quat(CCamera::CreateOrientationYPR(ypr));

	// Send updated transform to the entity, only orientation changes
	GetEntity()->SetPosRotScale(GetEntity()->GetWorldPos(), correctedOrientation, Vec3(1, 1, 1));
}

void CPlayerComponent::UpdateCamera(float frameTime)
{
	// Start with updating look orientation from the latest input
	Ang3 ypr = CCamera::CreateAnglesYPR(Matrix33(m_lookOrientation));

	ypr.x += m_mouseDeltaRotation.x * m_rotationSpeed;

	const float rotationLimitsMinPitch = -0.84f;
	const float rotationLimitsMaxPitch = 1.5f;

	// TODO: Perform soft clamp here instead of hard wall, should reduce rot speed in this direction when close to limit.
	ypr.y = CLAMP(ypr.y + m_mouseDeltaRotation.y * m_rotationSpeed, rotationLimitsMinPitch, rotationLimitsMaxPitch);
	// Skip roll
	if (m_bSliding) {
		ypr.z = m_TiltAngle;
	}
	else {
		ypr.z = 0;
	}

	m_lookOrientation = Quat(CCamera::CreateOrientationYPR(ypr));

	// Reset every frame
	m_mouseDeltaRotation = ZERO;

	// Ignore z-axis rotation, that's set by CPlayerAnimations
	ypr.x = 0;
	
	// Start with changing view rotation to the requested mouse look orientation
	Matrix34 localTransform = IDENTITY;
	localTransform.SetRotation33(CCamera::CreateOrientationYPR(ypr));
	localTransform.SetTranslation(Vec3(0,0,1.9));
	const float viewOffsetForward = 0.01f;

	m_pCameraComponent->SetTransformMatrix(localTransform);
	m_pAudioListenerComponent->SetOffset(localTransform.GetTranslation());
}

void CPlayerComponent::OnReadyForGameplayOnServer()
{
	CRY_ASSERT(gEnv->bServer, "This function should only be called on the server!");

	const Matrix34 newTransform = CSpawnPointComponent::GetFirstSpawnPointTransform();
	
	Revive(newTransform);
	
	// Invoke the RemoteReviveOnClient function on all remote clients, to ensure that Revive is called across the network
	SRmi<RMI_WRAP(&CPlayerComponent::RemoteReviveOnClient)>::InvokeOnOtherClients(this, RemoteReviveParams{ newTransform.GetTranslation(), Quat(newTransform) });

	// Go through all other players, and send the RemoteReviveOnClient on their instances to the new player that is ready for gameplay
	const int channelId = m_pEntity->GetNetEntity()->GetChannelId();
	CGamePlugin::GetInstance()->IterateOverPlayers([this, channelId](CPlayerComponent& player)
	{
		// Don't send the event for the player itself (handled in the RemoteReviveOnClient event above sent to all clients)
		if (player.GetEntityId() == GetEntityId())
			return;

		// Only send the Revive event to players that have already respawned on the server
		if (!player.m_isAlive)
			return;

		// Revive this player on the new player's machine, on the location the existing player was currently at
		const QuatT currentOrientation = QuatT(player.GetEntity()->GetWorldTM());
		SRmi<RMI_WRAP(&CPlayerComponent::RemoteReviveOnClient)>::InvokeOnClient(&player, RemoteReviveParams{ currentOrientation.t, currentOrientation.q }, channelId);
	});
}

bool CPlayerComponent::RemoteReviveOnClient(RemoteReviveParams&& params, INetChannel* pNetChannel)
{
	// Call the Revive function on this client
	Revive(Matrix34::Create(Vec3(1.f), params.rotation, params.position));

	return true;
}

void CPlayerComponent::Revive(const Matrix34& transform)
{
	m_isAlive = true;
	
	// Set the entity transformation, except if we are in the editor
	// In the editor case we always prefer to spawn where the viewport is
	if(!gEnv->IsEditor())
	{
		m_pEntity->SetWorldTM(transform);
	}
	
	// Apply the character to the entity and queue animations
	m_pCharacterController->Physicalize();

	// Reset input now that the player respawned
	m_inputFlags.Clear();
	NetMarkAspectsDirty(InputAspect);
	
	m_mouseDeltaRotation = ZERO;
	m_lookOrientation = IDENTITY;

	m_mouseDeltaSmoothingFilter.Reset();

	m_activeFragmentId = FRAGMENT_ID_INVALID;

	m_horizontalAngularVelocity = 0.0f;
	m_averagedHorizontalAngularVelocity.Reset();

}

void CPlayerComponent::HandleInputFlagChange(const CEnumFlags<EInputFlag> flags, const CEnumFlags<EActionActivationMode> activationMode, const EInputFlagType type)
{
	switch (type)
	{
	case EInputFlagType::Hold:
	{
		if (activationMode == eAAM_OnRelease)
		{
			m_inputFlags &= ~flags;
		}
		else
		{
			m_inputFlags |= flags;
		}
	}
	break;
	case EInputFlagType::Toggle:
	{
		if (activationMode == eAAM_OnRelease)
		{
			// Toggle the bit(s)
			m_inputFlags ^= flags;
		}
	}
	break;
	}
	
	if(IsLocalClient())
	{
		NetMarkAspectsDirty(InputAspect);
	}
}
