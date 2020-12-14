#pragma once

#include <array>
#include <numeric>

#include <CryEntitySystem/IEntityComponent.h>
#include <CryMath/Cry_Camera.h>

#include <ICryMannequin.h>
#include <CrySchematyc/Utils/EnumFlags.h>

#include <DefaultComponents/Cameras/CameraComponent.h>
#include <DefaultComponents/Physics/CharacterControllerComponent.h>
#include <DefaultComponents/Geometry/AdvancedAnimationComponent.h>
#include <DefaultComponents/Input/InputComponent.h>
#include <DefaultComponents/Audio/ListenerComponent.h>

////////////////////////////////////////////////////////
// Represents a player participating in gameplay
////////////////////////////////////////////////////////

struct Cmd
{
	float forwardMove;
	float rightMove;
	float upMove;
};

class CPlayerComponent final : public IEntityComponent
{
	

	enum class EInputFlagType
	{
		Hold = 0,
		Toggle
	};

	enum class EInputFlag : uint8
	{
		MoveLeft = 1 << 0,
		MoveRight = 1 << 1,
		MoveForward = 1 << 2,
		MoveBack = 1 << 3,
		Jump = 1 << 4
	};
	
	static constexpr EEntityAspects InputAspect = eEA_GameClientD;

	template<typename T, size_t SAMPLES_COUNT>
	class MovingAverage
	{
		static_assert(SAMPLES_COUNT > 0, "SAMPLES_COUNT shall be larger than zero!");

	public:

		MovingAverage()
			: m_values()
			, m_cursor(SAMPLES_COUNT)
			, m_accumulator()
		{
		}

		MovingAverage& Push(const T& value)
		{
			if (m_cursor == SAMPLES_COUNT)
			{
				m_values.fill(value);
				m_cursor = 0;
				m_accumulator = std::accumulate(m_values.begin(), m_values.end(), T(0));
			}
			else
			{
				m_accumulator -= m_values[m_cursor];
				m_values[m_cursor] = value;
				m_accumulator += m_values[m_cursor];
				m_cursor = (m_cursor + 1) % SAMPLES_COUNT;
			}

			return *this;
		}

		T Get() const
		{
			return m_accumulator / T(SAMPLES_COUNT);
		}

		void Reset()
		{
			m_cursor = SAMPLES_COUNT;
		}

	private:

		std::array<T, SAMPLES_COUNT> m_values;
		size_t m_cursor;

		T m_accumulator;
	};

public:
	CPlayerComponent() = default;
	virtual ~CPlayerComponent() = default;

	// IEntityComponent
	virtual void Initialize() override;

	virtual Cry::Entity::EventFlags GetEventMask() const override;
	virtual void ProcessEvent(const SEntityEvent& event) override;
	
	virtual bool NetSerialize(TSerialize ser, EEntityAspects aspect, uint8 profile, int flags) override;
	virtual NetworkAspectType GetNetSerializeAspectMask() const override { return InputAspect; }
	// ~IEntityComponent

	// Reflect type to set a unique identifier for this component
	static void ReflectType(Schematyc::CTypeDesc<CPlayerComponent>& desc)
	{
		desc.SetGUID("{63F4C0C6-32AF-4ACB-8FB0-57D45DD14725}"_cry_guid);
	}

	void OnReadyForGameplayOnServer();
	bool IsLocalClient() const { return (m_pEntity->GetFlags() & ENTITY_FLAG_LOCAL_PLAYER) != 0; }

protected:
	void Revive(const Matrix34& transform);

	void SetMovementDir();
	void QueueJump();
	void AirMove();
	void Accelerate(Vec3 wishdir, float wishspeed, float accel);
	void AirControl(Vec3 wishdir, float wishspeed);
	void GroundMove();
	void ApplyFriction(float t);
	void UpdateLookDirectionRequest(float frameTime);
	void UpdateAnimation(float frameTime);
	void UpdateLookRotationZ(float frameTime);
	void UpdateCamera(float frameTime);
	void Update(float frameTime);
	void HandleInputFlagChange(CEnumFlags<EInputFlag> flags, CEnumFlags<EActionActivationMode> activationMode, EInputFlagType type = EInputFlagType::Hold);

	// Called when this entity becomes the local player, to create client specific setup such as the Camera
	void InitializeLocalPlayer();


	
	// Start remote method declarations
protected:
	// Parameters to be passed to the RemoteReviveOnClient function
	struct RemoteReviveParams
	{
		// Called once on the server to serialize data to the other clients
		// Then called once on the other side to deserialize
		void SerializeWith(TSerialize ser)
		{
			// Serialize the position with the 'wrld' compression policy
			ser.Value("pos", position, 'wrld');
			// Serialize the rotation with the 'ori0' compression policy
			ser.Value("rot", rotation, 'ori0');
		}
		
		Vec3 position;
		Quat rotation;
	};
	// Remote method intended to be called on all remote clients when a player spawns on the server
	bool RemoteReviveOnClient(RemoteReviveParams&& params, INetChannel* pNetChannel);
	
protected:
	bool m_isAlive = false;

	Cry::DefaultComponents::CCameraComponent* m_pCameraComponent = nullptr;
	Cry::DefaultComponents::CCharacterControllerComponent* m_pCharacterController = nullptr;
	Cry::DefaultComponents::CInputComponent* m_pInputComponent = nullptr;
	Cry::Audio::DefaultComponents::CListenerComponent* m_pAudioListenerComponent = nullptr;

	FragmentID m_idleFragmentId;
	FragmentID m_walkFragmentId;
	TagID m_rotateTagId;

	CEnumFlags<EInputFlag> m_inputFlags;
	Vec2 m_mouseDeltaRotation;
	MovingAverage<Vec2, 10> m_mouseDeltaSmoothingFilter;
	float m_TiltAngle = 0.26;
	bool m_bSliding = false;
	bool m_bSprinting = false;
	float m_walkSpeed = 20.5;
	float m_sprintSpeed = 41;
	float m_JumpForce = 500;
	float m_ViewOffsetUp = 0.26f;
	float m_frametime;
	float m_StandingViewOffset = 0.26f;
	float m_SlidingViewOffset = 0.05f;
	float m_CrouchingViewOffset = 0.1f;
	CryTransform::CAngle m_sprintFOV = 95_degrees;
	CryTransform::CAngle m_defaultFOV = 90_degrees;
	float m_moveSpeed = 1000;

	 float gravity = 2000;

	 float friction = 6; //Ground friction

	/* Movement stuff */
	 float moveSpeed = 70;                // Ground move speed
	 float runAcceleration = 140;         // Ground accel
	 float runDeacceleration = 600;       // Deacceleration that occurs when running on the ground
	 float airAcceleration = 0.3;        // Air accel
	 float airDecceleration = 0.3;

       // Deacceleration experienced when ooposite strafing
	 float airControl = 1;               // How precise air control is
	 float sideStrafeAcceleration = 5;  // How fast acceleration occurs to get up to sideStrafeSpeed when
	 float sideStrafeSpeed = 10;          // What the max speed to generate when side strafing
	 float jumpSpeed = 80;                // The speed at which the character's up axis gains when hitting jump
	 bool holdJumpToBhop = true;           // When enabled allows player to just hold jump button to keep on bhopping perfectly. Beware: smells like casual.

	 bool wishJump;
	 float playerFriction;
	 Vec3 moveDirectionNorm = ZERO;
	 Vec3 playerVelocity = ZERO;
	 
	 Cmd _cmd;


	const float m_rotationSpeed = 0.002f;

	int m_cameraJointId = -1;

	FragmentID m_activeFragmentId;

	Quat m_lookOrientation; //!< Should translate to head orientation in the future
	float m_horizontalAngularVelocity;
	MovingAverage<float, 10> m_averagedHorizontalAngularVelocity;
};
