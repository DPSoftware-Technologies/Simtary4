#pragma once
// Native Component system for the Simtary engine.
//	A Unity-like component model for C++: attach C++ classes to scene entities ("GameObjects")
//	and drive them with Start()/Update()/Destroy() lifecycle callbacks.
//
//	Attachment is data-driven through the engine's MetadataComponent so it stays fully
//	compatible with the Wicked Editor. Two metadata conventions are used:
//
//		NCI_<LocalID>                            = "ComponentName"   (string)
//			"Native Component Import": attaches the component named ComponentName to the entity.
//			LocalID lets you stack several instances of the same component on one entity.
//
//		NCA_<LocalID>_<ArgName>                  = value             (bool / int / float / string)
//			"Native Component Argument": a parameter handed to that component instance.
//			(LocalID already identifies the component, so the component name is not repeated.)
//
//	Example (matches the editor screenshot):
//		NCI_0          = "MyClass1"
//		NCA_0_Args1    = "Hello"   (string)
//		NCA_0_Args2    = 123       (int)
//		NCA_0_Args3    = 0.542     (float)
//		NCA_0_Args4    = true      (bool)
//
//	Define a component:
//		struct MyClass1 : wi::scene::NativeComponent
//		{
//			float speed = 1.0f;
//			void Start()  override { Bind(speed, "Args3"); }
//			void Update(float dt) override
//			{
//				TransformComponent* tr = GetComponent<TransformComponent>(); // engine component on same entity
//				if (tr) tr->Translate(XMFLOAT3(0, speed * dt, 0));
//			}
//		};
//		ST_REGISTER_NATIVE_COMPONENT(MyClass1) // put this in a .cpp file

#include "wiECS.h"
#include "wiVector.h"
#include "wiUnorderedMap.h"
#include "wiJobSystem.h"

#include <string>
#include <memory>
#include <functional>
#include <type_traits>

namespace wi::scene
{
	struct Scene; // forward declaration; full definition lives in wiScene.h

	// Lightweight compile-time type identity. RTTI is disabled engine-wide (/GR-, -fno-rtti)
	// so typeid/dynamic_cast are unavailable; this gives each type a unique stable address instead.
	using NativeTypeID = const void*;
	template<typename T>
	inline NativeTypeID GetNativeTypeID()
	{
		static const char id = 0;
		return &id;
	}

	// Base class for a native component. Derive from this and override the lifecycle methods.
	//	The engine fills in scene/entity/localID/componentName before Start() is called.
	struct NativeComponent
	{
		// Runtime context (set by the engine, valid from Start() onwards):
		Scene* scene = nullptr;                            // owning scene
		wi::ecs::Entity entity = wi::ecs::INVALID_ENTITY;  // owning entity ("GameObject")
		int localID = 0;                                   // the <LocalID> from NCI_<LocalID>
		std::string componentName;                         // registered name (NCI_ value)

		NativeComponent() = default;
		virtual ~NativeComponent() = default;

		// Lifecycle (override in your subclass):
		virtual void Start() {}            // called once, the first time the component is seen
		virtual void Update(float dt) {}   // called every frame while dt > 0
		virtual void Destroy() {}          // called once when removed / entity removed / scene cleared

		// Parameter access. Reads NCA_<localID>_<name> from the entity's MetadataComponent.
		bool        GetBool(const std::string& name, bool def = false) const;
		int         GetInt(const std::string& name, int def = 0) const;
		float       GetFloat(const std::string& name, float def = 0.0f) const;
		std::string GetString(const std::string& name, const std::string& def = "") const;
		bool        HasParam(const std::string& name) const;

		// One-line binding of a member field to a parameter (type deduced from the field):
		//	Bind(speed, "Args3");  // same as: speed = GetFloat("Args3", speed);
		template<typename T>
		void Bind(T& field, const std::string& name)
		{
			if constexpr (std::is_same_v<T, bool>)                          field = GetBool(name, field);
			else if constexpr (std::is_same_v<T, std::string>)              field = GetString(name, field);
			else if constexpr (std::is_floating_point_v<T>)                 field = (T)GetFloat(name, (float)field);
			else if constexpr (std::is_integral_v<T> || std::is_enum_v<T>)  field = (T)GetInt(name, (int)field);
			else static_assert(!sizeof(T*), "Bind: unsupported field type (use bool/int/float/std::string).");
		}

		// GetComponent<T>() — Unity-style lookup on the SAME entity:
		//	- if T is an engine component (TransformComponent, MeshComponent, ...) returns it (or nullptr)
		//	- if T derives from NativeComponent returns the first native instance of that type (or nullptr)
		template<typename T> T* GetComponent();
		// Native-only: get the instance attached with a specific LocalID (for stacked components).
		template<typename T> T* GetComponentByID(int id);
		// Native-only: append every native instance of type T on this entity to 'out'.
		template<typename T> void GetComponents(wi::vector<T*>& out);

		// non-copyable (owns no copyable state by contract; instances are heap-owned by the manager)
		NativeComponent(const NativeComponent&) = delete;
		NativeComponent& operator=(const NativeComponent&) = delete;
	};

	// Factory + type identity stored per registered component name.
	using NativeComponentFactory = std::function<std::unique_ptr<NativeComponent>()>;
	struct NativeComponentRegistration
	{
		NativeComponentFactory factory;
		NativeTypeID typeID = nullptr;
	};

	// Register a component type so it can be attached from metadata.
	//	Prefer the ST_REGISTER_NATIVE_COMPONENT macro below.
	void RegisterNativeComponent(const std::string& name, NativeComponentFactory factory, NativeTypeID typeID);
	// Look up a registration by the metadata name (returns nullptr if not registered).
	const NativeComponentRegistration* FindNativeComponentRegistration(const std::string& name);

	// Per-scene runtime state for native components.
	//	This is NOT serialized: attachments are rebuilt from the (serialized) MetadataComponent every Update.
	struct NativeComponentManager
	{
		struct Instance
		{
			std::unique_ptr<NativeComponent> component;
			NativeTypeID typeID = nullptr;
			int localID = 0;
			std::string name;
			bool started = false;
		};
		wi::unordered_map<wi::ecs::Entity, wi::vector<Instance>> instances; // entity -> attached instances

		// Reconcile attachments against metadata, fire Start() on new ones and Update(dt) on all.
		//	Called by Scene::RunNativeComponentUpdateSystem once per frame.
		void RunUpdate(Scene& scene, float dt);
		// Destroy + drop every instance on one entity (called from Scene::Entity_Remove).
		void RemoveEntity(wi::ecs::Entity entity);
		// Destroy + drop everything (called from Scene::Clear).
		void Clear();

		// Lookups used by NativeComponent::GetComponent / GetComponentByID / GetComponents:
		NativeComponent* Get(wi::ecs::Entity entity, NativeTypeID typeID) const;
		NativeComponent* GetByID(wi::ecs::Entity entity, NativeTypeID typeID, int localID) const;
		void GetAll(wi::ecs::Entity entity, NativeTypeID typeID, wi::vector<NativeComponent*>& out) const;
	};
}

// Register a native component under its own type name (the string used in NCI_<id>).
//	Place in a .cpp file:  ST_REGISTER_NATIVE_COMPONENT(MyClass1)
#define ST_REGISTER_NATIVE_COMPONENT(TYPE) \
	namespace { struct TYPE##_NativeReg { TYPE##_NativeReg() { \
		::wi::scene::RegisterNativeComponent(#TYPE, []() { \
			return std::unique_ptr<::wi::scene::NativeComponent>(new TYPE()); }, \
			::wi::scene::GetNativeTypeID<TYPE>()); \
	} }; static TYPE##_NativeReg _global_##TYPE##_NativeReg_instance; }

// Register a native component under a custom name (when the NCI_ string differs from the C++ type name).
//	Place in a .cpp file:  ST_REGISTER_NATIVE_COMPONENT_AS(MyClass1, "Spinner")
#define ST_REGISTER_NATIVE_COMPONENT_AS(TYPE, NAME) \
	namespace { struct TYPE##_NativeRegAs { TYPE##_NativeRegAs() { \
		::wi::scene::RegisterNativeComponent(NAME, []() { \
			return std::unique_ptr<::wi::scene::NativeComponent>(new TYPE()); }, \
			::wi::scene::GetNativeTypeID<TYPE>()); \
	} }; static TYPE##_NativeRegAs _global_##TYPE##_NativeRegAs_instance; }
