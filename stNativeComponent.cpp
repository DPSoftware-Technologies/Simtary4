#include "stNativeComponent.h"
#include "wiScene.h"
#include "wiBacklog.h"
#include "wiProfiler.h"

#include <cstdlib>

using namespace wi::ecs;

namespace wi::scene
{
	// ------------------------------------------------------------------
	// Global registry (component name -> factory)
	// ------------------------------------------------------------------
	static wi::unordered_map<std::string, NativeComponentRegistration>& GetRegistry()
	{
		// Function-local static so it is initialized before any ST_REGISTER_NATIVE_COMPONENT
		// static initializer that runs in another translation unit.
		static wi::unordered_map<std::string, NativeComponentRegistration> registry;
		return registry;
	}

	void RegisterNativeComponent(const std::string& name, NativeComponentFactory factory, NativeTypeID typeID)
	{
		NativeComponentRegistration& reg = GetRegistry()[name];
		reg.factory = std::move(factory);
		reg.typeID = typeID;
	}

	const NativeComponentRegistration* FindNativeComponentRegistration(const std::string& name)
	{
		auto& registry = GetRegistry();
		auto it = registry.find(name);
		if (it == registry.end())
			return nullptr;
		return &it->second;
	}

	// ------------------------------------------------------------------
	// Parameter access (reads NCA_<localID>_<name> from metadata)
	// ------------------------------------------------------------------
	static const MetadataComponent* GetMetadata(const NativeComponent* self)
	{
		if (self->scene == nullptr)
			return nullptr;
		return self->scene->metadatas.GetComponent(self->entity);
	}
	static std::string ArgKey(const NativeComponent* self, const std::string& name)
	{
		return "NCA_" + std::to_string(self->localID) + "_" + name;
	}

	bool NativeComponent::GetBool(const std::string& name, bool def) const
	{
		const MetadataComponent* m = GetMetadata(this);
		if (m == nullptr)
			return def;
		const std::string key = ArgKey(this, name);
		return m->bool_values.has(key) ? m->bool_values.get(key) : def;
	}
	int NativeComponent::GetInt(const std::string& name, int def) const
	{
		const MetadataComponent* m = GetMetadata(this);
		if (m == nullptr)
			return def;
		const std::string key = ArgKey(this, name);
		return m->int_values.has(key) ? m->int_values.get(key) : def;
	}
	float NativeComponent::GetFloat(const std::string& name, float def) const
	{
		const MetadataComponent* m = GetMetadata(this);
		if (m == nullptr)
			return def;
		const std::string key = ArgKey(this, name);
		return m->float_values.has(key) ? m->float_values.get(key) : def;
	}
	std::string NativeComponent::GetString(const std::string& name, const std::string& def) const
	{
		const MetadataComponent* m = GetMetadata(this);
		if (m == nullptr)
			return def;
		const std::string key = ArgKey(this, name);
		return m->string_values.has(key) ? m->string_values.get(key) : def;
	}
	bool NativeComponent::HasParam(const std::string& name) const
	{
		const MetadataComponent* m = GetMetadata(this);
		if (m == nullptr)
			return false;
		const std::string key = ArgKey(this, name);
		return m->bool_values.has(key) || m->int_values.has(key) ||
			m->float_values.has(key) || m->string_values.has(key);
	}

	// ------------------------------------------------------------------
	// NativeComponentManager
	// ------------------------------------------------------------------
	void NativeComponentManager::RunUpdate(Scene& scene, float dt)
	{
		auto range = wi::profiler::BeginRangeCPU("Native Components");

		// --- Pass A: prune instances that no longer match the metadata ---
		wi::vector<Entity> empty_entities;
		for (auto& pair : instances)
		{
			const Entity entity = pair.first;
			wi::vector<NativeComponentManager::Instance>& list = pair.second;
			const MetadataComponent* meta = scene.metadatas.GetComponent(entity);

			for (size_t i = list.size(); i-- > 0; )
			{
				NativeComponentManager::Instance& inst = list[i];
				bool keep = false;
				if (meta != nullptr)
				{
					const std::string key = "NCI_" + std::to_string(inst.localID);
					if (meta->string_values.has(key) && meta->string_values.get(key) == inst.name)
						keep = true;
				}
				if (!keep)
				{
					if (inst.component)
						inst.component->Destroy();
					list.erase(list.begin() + i);
				}
			}

			if (list.empty())
				empty_entities.push_back(entity);
		}
		for (Entity e : empty_entities)
			instances.erase(e);

		// --- Pass B: create instances for NCI_<id> keys that aren't attached yet ---
		for (size_t mi = 0; mi < scene.metadatas.GetCount(); ++mi)
		{
			const Entity entity = scene.metadatas.GetEntity(mi);
			const MetadataComponent& meta = scene.metadatas[mi];

			for (size_t k = 0; k < meta.string_values.names.size(); ++k)
			{
				const std::string& key = meta.string_values.names[k];
				if (key.rfind("NCI_", 0) != 0) // not an import key
					continue;

				const std::string idstr = key.substr(4);
				if (idstr.empty())
					continue;
				const int localID = std::atoi(idstr.c_str());
				const std::string& compName = meta.string_values.values[k];
				if (compName.empty())
					continue;

				// Skip if already attached with this localID:
				bool exists = false;
				auto it = instances.find(entity);
				if (it != instances.end())
				{
					for (const NativeComponentManager::Instance& inst : it->second)
					{
						if (inst.localID == localID)
						{
							exists = true;
							break;
						}
					}
				}
				if (exists)
					continue;

				const NativeComponentRegistration* reg = FindNativeComponentRegistration(compName);
				if (reg == nullptr || !reg->factory)
				{
					wi::backlog::post(
						"NativeComponent: '" + compName + "' (NCI_" + idstr + ") is not registered. "
						"Did you add ST_REGISTER_NATIVE_COMPONENT(" + compName + ")?",
						wi::backlog::LogLevel::Warning);
					continue;
				}

				NativeComponentManager::Instance inst;
				inst.component = reg->factory();
				if (!inst.component)
					continue;
				inst.typeID = reg->typeID;
				inst.localID = localID;
				inst.name = compName;
				inst.started = false;
				inst.component->scene = &scene;
				inst.component->entity = entity;
				inst.component->localID = localID;
				inst.component->componentName = compName;
				instances[entity].push_back(std::move(inst));
			}
		}

		// --- Pass C: Start() newly created instances, Update(dt) all of them ---
		//	Single-threaded on purpose: user code may freely touch the scene.
		for (auto& pair : instances)
		{
			wi::vector<NativeComponentManager::Instance>& list = pair.second;
			for (NativeComponentManager::Instance& inst : list)
			{
				if (!inst.component)
					continue;
				if (!inst.started)
				{
					inst.component->Start();
					inst.started = true;
				}
				if (dt > 0)
					inst.component->Update(dt);
			}
		}

		wi::profiler::EndRange(range);
	}

	void NativeComponentManager::RemoveEntity(Entity entity)
	{
		auto it = instances.find(entity);
		if (it == instances.end())
			return;
		for (NativeComponentManager::Instance& inst : it->second)
		{
			if (inst.component)
				inst.component->Destroy();
		}
		instances.erase(it);
	}

	void NativeComponentManager::Clear()
	{
		for (auto& pair : instances)
		{
			for (NativeComponentManager::Instance& inst : pair.second)
			{
				if (inst.component)
					inst.component->Destroy();
			}
		}
		instances.clear();
	}

	NativeComponent* NativeComponentManager::Get(Entity entity, NativeTypeID typeID) const
	{
		auto it = instances.find(entity);
		if (it == instances.end())
			return nullptr;
		for (const NativeComponentManager::Instance& inst : it->second)
		{
			if (inst.typeID == typeID)
				return inst.component.get();
		}
		return nullptr;
	}

	NativeComponent* NativeComponentManager::GetByID(Entity entity, NativeTypeID typeID, int localID) const
	{
		auto it = instances.find(entity);
		if (it == instances.end())
			return nullptr;
		for (const NativeComponentManager::Instance& inst : it->second)
		{
			if (inst.localID == localID && inst.typeID == typeID)
				return inst.component.get();
		}
		return nullptr;
	}

	void NativeComponentManager::GetAll(Entity entity, NativeTypeID typeID, wi::vector<NativeComponent*>& out) const
	{
		auto it = instances.find(entity);
		if (it == instances.end())
			return;
		for (const NativeComponentManager::Instance& inst : it->second)
		{
			if (inst.typeID == typeID)
				out.push_back(inst.component.get());
		}
	}

	// ------------------------------------------------------------------
	// Scene hook
	// ------------------------------------------------------------------
	void Scene::RunNativeComponentUpdateSystem(wi::jobsystem::context& ctx)
	{
		nativeComponents.RunUpdate(*this, dt);
	}
}
