#include "asset_browser.h"
#include "audio/audio_device.h"
#include "audio/audio_system.h"
#include "audio/clip_manager.h"
#include "core/crc32.h"
#include "core/FS/file_system.h"
#include "core/FS/ifile.h"
#include "core/json_serializer.h"
#include "core/log.h"
#include "core/path_utils.h"
#include "core/profiler.h"
#include "core/resource.h"
#include "core/resource_manager.h"
#include "core/resource_manager_base.h"
#include "core/string.h"
#include "core/system.h"
#include "editor/ieditor_command.h"
#include "editor/world_editor.h"
#include "engine/engine.h"
#include "engine/iplugin.h"
#include "engine/plugin_manager.h"
#include "file_system_watcher.h"
#include "gui_interface.h"
#include "lua_script/lua_script_manager.h"
#include "metadata.h"
#include "platform_interface.h"
#include "renderer/material.h"
#include "renderer/model.h"
#include "renderer/render_scene.h"
#include "renderer/shader.h"
#include "renderer/texture.h"
#include "universe/universe.h"
#include "utils.h"


static const Lumix::uint32 UNIVERSE_HASH = Lumix::crc32("universe");
static const Lumix::uint32 SOURCE_HASH = Lumix::crc32("source");
static const Lumix::uint32 CLIP_HASH = Lumix::crc32("CLIP");
static const Lumix::uint32 LUA_SCRIPT_HASH = Lumix::crc32("lua_script");


static Lumix::uint32 getResourceType(const char* path)
{
	char ext[10];
	Lumix::PathUtils::getExtension(ext, sizeof(ext), path);

	if (Lumix::compareString(ext, "ogg") == 0) return CLIP_HASH;
	if (Lumix::compareString(ext, "mat") == 0) return Lumix::ResourceManager::MATERIAL;
	if (Lumix::compareString(ext, "msh") == 0) return Lumix::ResourceManager::MODEL;
	if (Lumix::compareString(ext, "dds") == 0) return Lumix::ResourceManager::TEXTURE;
	if (Lumix::compareString(ext, "raw") == 0) return Lumix::ResourceManager::TEXTURE;
	if (Lumix::compareString(ext, "tga") == 0) return Lumix::ResourceManager::TEXTURE;
	if (Lumix::compareString(ext, "shd") == 0) return Lumix::ResourceManager::SHADER;
	if (Lumix::compareString(ext, "unv") == 0) return UNIVERSE_HASH;
	if (Lumix::compareString(ext, "lua") == 0) return LUA_SCRIPT_HASH;

	return 0;
}


class InsertMeshCommand : public Lumix::IEditorCommand
{
public:
	InsertMeshCommand(Lumix::WorldEditor& editor);
	InsertMeshCommand(Lumix::WorldEditor& editor, const Lumix::Vec3& position, const Lumix::Path& mesh_path);

	void serialize(Lumix::JsonSerializer& serializer) override;
	void deserialize(Lumix::JsonSerializer& serializer) override;
	bool execute() override;
	void undo() override;
	Lumix::uint32 getType() override;
	virtual bool merge(IEditorCommand&);
	Lumix::Entity getEntity() const { return m_entity; }

private:
	Lumix::Vec3 m_position;
	Lumix::Path m_mesh_path;
	Lumix::Entity m_entity;
	Lumix::WorldEditor& m_editor;
};


static Lumix::IEditorCommand* createInsertMeshCommand(Lumix::WorldEditor& editor)
{
	return LUMIX_NEW(editor.getAllocator(), InsertMeshCommand)(editor);
}


AssetBrowser::AssetBrowser(Lumix::WorldEditor& editor, Metadata& metadata)
	: m_editor(editor)
	, m_metadata(metadata)
	, m_resources(editor.getAllocator())
	, m_selected_resource(nullptr)
	, m_autoreload_changed_resource(true)
	, m_changed_files(editor.getAllocator())
	, m_is_focus_requested(false)
	, m_changed_files_mutex(false)
	, m_history(editor.getAllocator())
	, m_playing_clip(nullptr)
	, m_gui(nullptr)
{
	m_filter[0] = '\0';
	m_current_type = 0;
	m_text_buffer[0] = '\0';
	m_is_opened = false;
	for (int i = 0; i < Count; ++i)
	{
		m_resources.emplace(editor.getAllocator());
	}

	findResources();

	m_watcher = FileSystemWatcher::create(editor.getBasePath(), editor.getAllocator());
	m_watcher->getCallback().bind<AssetBrowser, &AssetBrowser::onFileChanged>(this);

	m_editor.registerEditorCommandCreator("insert_mesh", createInsertMeshCommand);
}


AssetBrowser::~AssetBrowser()
{
	unloadResource();
	FileSystemWatcher::destroy(m_watcher);
}


void AssetBrowser::onFileChanged(const char* path)
{
	Lumix::uint32 resource_type = getResourceType(path);
	if (resource_type == 0) return;

	Lumix::MT::SpinLock lock(m_changed_files_mutex);
	m_changed_files.push(Lumix::Path(path));
}


void AssetBrowser::unloadResource()
{
	if (!m_selected_resource) return;

	stopAudio();
	m_selected_resource->getResourceManager()
		.get(getResourceType(m_selected_resource->getPath().c_str()))
		->unload(*m_selected_resource);

	m_selected_resource = nullptr;
}


AssetBrowser::Type AssetBrowser::getTypeFromResourceManagerType(Lumix::uint32 type) const
{
	switch (type)
	{
		case Lumix::ResourceManager::MODEL: return MODEL;
		case Lumix::ResourceManager::SHADER: return SHADER;
		case Lumix::ResourceManager::TEXTURE: return TEXTURE;
		case Lumix::ResourceManager::MATERIAL: return MATERIAL;
	}
	if (type == UNIVERSE_HASH) return UNIVERSE;
	if (type == LUA_SCRIPT_HASH) return LUA_SCRIPT;

	return MODEL;
}


void AssetBrowser::update()
{
	PROFILE_FUNCTION();
	bool is_empty;
	{
		Lumix::MT::SpinLock lock(m_changed_files_mutex);
		is_empty = m_changed_files.empty();
	}

	while (!is_empty)
	{
		Lumix::Path path_obj;
		{
			Lumix::MT::SpinLock lock(m_changed_files_mutex);

			path_obj = m_changed_files.back();
			m_changed_files.pop();
			is_empty = m_changed_files.empty();
		}

		const char* path = path_obj.c_str();

		Lumix::uint32 resource_type = getResourceType(path);
		if (resource_type == 0) continue;

		if (m_autoreload_changed_resource) m_editor.getEngine().getResourceManager().reload(path);

		if (!Lumix::fileExists(path))
		{
			int index = getTypeFromResourceManagerType(resource_type);
			m_resources[index].eraseItemFast(path_obj);
			continue;
		}

		char dir[Lumix::MAX_PATH_LENGTH];
		char filename[Lumix::MAX_PATH_LENGTH];
		Lumix::PathUtils::getDir(dir, sizeof(dir), path);
		Lumix::PathUtils::getFilename(filename, sizeof(filename), path);
		addResource(dir, filename);
	}
	m_changed_files.clear();
}


void AssetBrowser::setGUIInterface(GUIInterface& gui)
{
	m_gui = &gui;
}


void AssetBrowser::onGUI()
{
	if (m_wanted_resource.isValid())
	{
		selectResource(m_wanted_resource);
		m_wanted_resource = "";
	}

	if (!m_is_opened) return;

	if (!m_gui->begin("AssetBrowser", &m_is_opened))
	{
		m_gui->end();
		return;
	}

	if (m_is_focus_requested)
	{
		m_is_focus_requested = false;
		ImGui::SetWindowFocus();
	}

	if (m_gui->button("Refresh")) findResources();
	m_gui->sameLine();
	m_gui->checkbox("Autoreload", &m_autoreload_changed_resource);

	const char* items = "Material\0Model\0Shader\0Texture\0Universe\0Lua Script\0Audio\0";
	ImGui::Combo("Type", &m_current_type, items);
	m_gui->inputText("Filter", m_filter, sizeof(m_filter));

	ImGui::ListBoxHeader("Resources");
	auto& resources = m_resources[m_current_type];

	for (auto& resource : resources)
	{
		if (m_filter[0] != '\0' && strstr(resource.c_str(), m_filter) == nullptr) continue;

		bool is_selected = m_selected_resource ? m_selected_resource->getPath() == resource : false;
		if (ImGui::Selectable(resource.c_str(), is_selected))
		{
			selectResource(resource);
		}
	}
	ImGui::ListBoxFooter();
	onGUIResource();
	m_gui->end();
}


void AssetBrowser::selectResource(Lumix::Resource* resource)
{
	if (m_selected_resource) m_history.push(m_selected_resource->getPath());
	if (m_history.size() > 20) m_history.erase(0);


	m_text_buffer[0] = '\0';
	m_wanted_resource = "";
	unloadResource();
	m_selected_resource = resource;
	ASSERT(m_selected_resource->getRefCount() > 0);
}


void AssetBrowser::selectResource(const Lumix::Path& resource)
{
	char ext[30];
	Lumix::PathUtils::getExtension(ext, Lumix::lengthOf(ext), resource.c_str());
	if (Lumix::compareString(ext, "unv") == 0) return;

	auto& manager = m_editor.getEngine().getResourceManager();
	auto* resource_manager = manager.get(getResourceType(resource.c_str()));
	if (resource_manager) selectResource(resource_manager->load(resource));
}


void AssetBrowser::saveMaterial(Lumix::Material* material)
{
	Lumix::FS::FileSystem& fs = m_editor.getEngine().getFileSystem();
	// use temporary because otherwise the material is reloaded during saving
	char tmp_path[Lumix::MAX_PATH_LENGTH];
	strcpy(tmp_path, material->getPath().c_str());
	strcat(tmp_path, ".tmp");
	Lumix::FS::IFile* file =
		fs.open(fs.getDefaultDevice(), tmp_path, Lumix::FS::Mode::CREATE | Lumix::FS::Mode::WRITE);
	if (file)
	{
		Lumix::DefaultAllocator allocator;
		Lumix::JsonSerializer serializer(*file,
			Lumix::JsonSerializer::AccessMode::WRITE,
			material->getPath().c_str(),
			allocator);
		if (!material->save(serializer))
		{
			Lumix::g_log_error.log("Material manager") << "Error saving "
													   << material->getPath().c_str();
		}
		fs.close(*file);

		Lumix::deleteFile(material->getPath().c_str());
		Lumix::moveFile(tmp_path, material->getPath().c_str());
	}
	else
	{
		Lumix::g_log_error.log("Material manager") << "Could not save file "
												   << material->getPath().c_str();
	}
}


bool AssetBrowser::resourceInput(const char* label, const char* str_id, char* buf, int max_size, Type type)
{
	float item_w = ImGui::CalcItemWidth();
	auto& style = ImGui::GetStyle();
	ImGui::PushItemWidth(item_w - ImGui::CalcTextSize("...View").x - style.FramePadding.x * 4 -
						 style.ItemSpacing.x * 2);

	if (m_gui->inputText(StringBuilder<30>("##", str_id), buf, max_size)) return true;

	m_gui->sameLine();
	StringBuilder<50> popup_name("pu", str_id);
	if (m_gui->button(StringBuilder<30>("...##browse", str_id)))
	{
		ImGui::OpenPopup(popup_name);
	}
	m_gui->sameLine();
	if (m_gui->button(StringBuilder<30>("View##go", str_id)))
	{
		m_is_focus_requested = true;
		m_is_opened = true;
		m_wanted_resource = buf;
	}
	m_gui->sameLine();
	m_gui->text(label);
	ImGui::PopItemWidth();

	if (ImGui::BeginPopup(popup_name))
	{
		static char filter[128] = "";
		m_gui->inputText("Filter", filter, sizeof(filter));

		for (auto unv : getResources(type))
		{
			if (filter[0] != '\0' && strstr(unv.c_str(), filter) == nullptr) continue;

			if (ImGui::Selectable(unv.c_str(), false))
			{
				Lumix::copyString(buf, max_size, unv.c_str());
				ImGui::EndPopup();
				return true;
			}
		}

		ImGui::EndPopup();
	}

	return false;
}


void AssetBrowser::onGUIMaterial()
{
	auto* material = static_cast<Lumix::Material*>(m_selected_resource);

	if (m_gui->button("Save")) saveMaterial(material);
	m_gui->sameLine();
	if (m_gui->button("Open in external editor")) openInExternalEditor(material);

	bool b;
	if (material->hasAlphaCutoutDefine())
	{
		b = material->isAlphaCutout();
		if (m_gui->checkbox("Is alpha cutout", &b)) material->enableAlphaCutout(b);
	}

	b = material->isBackfaceCulling();
	if (m_gui->checkbox("Is backface culling", &b)) material->enableBackfaceCulling(b);

	if (material->hasShadowReceivingDefine())
	{
		b = material->isShadowReceiver();
		if (m_gui->checkbox("Is shadow receiver", &b)) material->enableShadowReceiving(b);
	}

	b = material->isZTest();
	if (m_gui->checkbox("Z test", &b)) material->enableZTest(b);

	Lumix::Vec3 specular = material->getSpecular();
	if (ImGui::ColorEdit3("Specular", &specular.x))
	{
		material->setSpecular(specular);
	}

	float shininess = material->getShininess();
	if (m_gui->dragFloat("Shininess", &shininess))
	{
		material->setShininess(shininess);
	}

	char buf[256];
	Lumix::copyString(buf, material->getShader() ? material->getShader()->getPath().c_str() : "");
	if (resourceInput("Shader", "shader", buf, sizeof(buf), SHADER))
	{
		material->setShader(Lumix::Path(buf));
	}

	for (int i = 0; i < material->getShader()->getTextureSlotCount(); ++i)
	{
		auto& slot = material->getShader()->getTextureSlot(i);
		auto* texture = material->getTexture(i);
		Lumix::copyString(buf, texture ? texture->getPath().c_str() : "");
		if (resourceInput(
				slot.m_name, StringBuilder<30>("", (Lumix::uint64)&slot), buf, sizeof(buf), TEXTURE))
		{
			material->setTexturePath(i, Lumix::Path(buf));
		}
		if (!texture) continue;

		m_gui->sameLine();
		StringBuilder<100> popup_name("pu", (Lumix::uint64)texture, slot.m_name);
		if (m_gui->button(StringBuilder<100>("Advanced##adv", (Lumix::uint64)texture, slot.m_name)))
		{
			ImGui::OpenPopup(popup_name);
		}

		if (ImGui::BeginPopup(popup_name))
		{
			bool u_clamp = (texture->getFlags() & BGFX_TEXTURE_U_CLAMP) != 0;
			if (m_gui->checkbox("u clamp", &u_clamp))
			{
				texture->setFlag(BGFX_TEXTURE_U_CLAMP, u_clamp);
			}
			bool v_clamp = (texture->getFlags() & BGFX_TEXTURE_V_CLAMP) != 0;
			if (m_gui->checkbox("v clamp", &v_clamp))
			{
				texture->setFlag(BGFX_TEXTURE_V_CLAMP, v_clamp);
			}
			bool min_point = (texture->getFlags() & BGFX_TEXTURE_MIN_POINT) != 0;
			if (m_gui->checkbox("Min point", &min_point))
			{
				texture->setFlag(BGFX_TEXTURE_MIN_POINT, min_point);
			}
			bool mag_point = (texture->getFlags() & BGFX_TEXTURE_MAG_POINT) != 0;
			if (m_gui->checkbox("Mag point", &mag_point))
			{
				texture->setFlag(BGFX_TEXTURE_MAG_POINT, mag_point);
			}
			if (slot.m_is_atlas)
			{
				int size = texture->getAtlasSize() - 2;
				const char values[] = { '2', 'x', '2', 0, '3', 'x', '3', 0, '4', 'x', '4', 0, 0 };
				if (ImGui::Combo(StringBuilder<30>("Atlas size##", i), &size, values))
				{
					texture->setAtlasSize(size + 2);
				}
			}
			ImGui::EndPopup();

		}

	}

	for (int i = 0; i < material->getUniformCount(); ++i)
	{
		auto& uniform = material->getUniform(i);
		switch (uniform.m_type)
		{
			case Lumix::Material::Uniform::FLOAT: 
				m_gui->dragFloat(uniform.m_name, &uniform.m_float);
				break;
		}
	}
	m_gui->columns(1);
}


void AssetBrowser::onGUITexture()
{
	auto* texture = static_cast<Lumix::Texture*>(m_selected_resource);
	if (texture->isFailure())
	{
		m_gui->text("Texture failed to load");
		return;
	}

	ImGui::LabelText("Size", "%dx%d", texture->getWidth(), texture->getHeight());
	ImGui::LabelText("BPP", "%d", texture->getBytesPerPixel());
	m_texture_handle = texture->getTextureHandle();
	if (bgfx::isValid(m_texture_handle))
	{
		ImGui::Image(&m_texture_handle, ImVec2(200, 200));
		if (m_gui->button("Open"))
		{
			openInExternalEditor(m_selected_resource);
		}
	}
}


Lumix::AudioDevice& getAudioDevice(Lumix::Engine& engine)
{
	auto* audio = static_cast<Lumix::AudioSystem*>(engine.getPluginManager().getPlugin("audio"));
	return audio->getDevice();
}


void AssetBrowser::stopAudio()
{
	if (m_playing_clip)
	{
		getAudioDevice(m_editor.getEngine()).stop(m_playing_clip);
		m_playing_clip = nullptr;
	}
}


void AssetBrowser::onGUIClip()
{
	auto* clip = static_cast<Lumix::Clip*>(m_selected_resource);
	ImGui::LabelText("Length", "%f", clip->getLengthSeconds());

	if (m_playing_clip && m_gui->button("Stop"))
	{
		stopAudio();
	}

	if (!m_playing_clip && m_gui->button("Play"))
	{
		stopAudio();

		auto& device = getAudioDevice(m_editor.getEngine());
		auto handle = device.createBuffer(
			clip->getData(), clip->getSize(), clip->getChannels(), clip->getSampleRate(), 0);
		device.play(handle, false);
		m_playing_clip = handle;
	}
}


void AssetBrowser::onGUILuaScript()
{
	auto* script = static_cast<Lumix::LuaScript*>(m_selected_resource);

	if (m_text_buffer[0] == '\0')
	{
		Lumix::copyString(m_text_buffer, script->getSourceCode());
	}
	ImGui::InputTextMultiline("Code", m_text_buffer, sizeof(m_text_buffer), ImVec2(0, 300));
	if (m_gui->button("Save"))
	{
		auto& fs = m_editor.getEngine().getFileSystem();
		auto* file = fs.open(fs.getDiskDevice(),
			m_selected_resource->getPath().c_str(),
			Lumix::FS::Mode::CREATE | Lumix::FS::Mode::WRITE);

		if (!file)
		{
			Lumix::g_log_warning.log("Asset browser") << "Could not save "
													  << m_selected_resource->getPath().c_str();
			return;
		}

		file->write(m_text_buffer, Lumix::stringLength(m_text_buffer));
		fs.close(*file);
	}
	m_gui->sameLine();
	if (m_gui->button("Open in external editor"))
	{
		openInExternalEditor(m_selected_resource);
	}
}


void AssetBrowser::openInExternalEditor(Lumix::Resource* resource)
{
	StringBuilder<Lumix::MAX_PATH_LENGTH> path(m_editor.getBasePath());
	path << "/" << resource->getPath().c_str();
	PlatformInterface::shellExecuteOpen(path);
}


void AssetBrowser::onGUIShader()
{
	auto* shader = static_cast<Lumix::Shader*>(m_selected_resource);
	StringBuilder<Lumix::MAX_PATH_LENGTH> path(m_editor.getBasePath());
	char basename[Lumix::MAX_PATH_LENGTH];
	Lumix::PathUtils::getBasename(
		basename, Lumix::lengthOf(basename), m_selected_resource->getPath().c_str());
	path << "/shaders/" << basename;
	if (m_gui->button("Open vertex shader"))
	{
		path << "_vs.sc";
		PlatformInterface::shellExecuteOpen(path);
	}
	m_gui->sameLine();
	if (m_gui->button("Open fragment shader"))
	{
		path << "_fs.sc";
		PlatformInterface::shellExecuteOpen(path);
	}

	if (m_gui->collapsingHeader("Texture slots", nullptr, true, true))
	{
		m_gui->columns(2);
		m_gui->text("name");
		m_gui->nextColumn();
		m_gui->text("uniform");
		m_gui->nextColumn();
		m_gui->separator();
		for (int i = 0; i < shader->getTextureSlotCount(); ++i)
		{
			auto& slot = shader->getTextureSlot(i);
			m_gui->text(slot.m_name);
			m_gui->nextColumn();
			m_gui->text(slot.m_uniform);
			m_gui->nextColumn();
		}
		m_gui->columns(1);
	}
}


static const Lumix::uint32 RENDERABLE_HASH = Lumix::crc32("renderable");


InsertMeshCommand::InsertMeshCommand(Lumix::WorldEditor& editor)
	: m_editor(editor)
{
}


InsertMeshCommand::InsertMeshCommand(Lumix::WorldEditor& editor,
	const Lumix::Vec3& position,
	const Lumix::Path& mesh_path)
	: m_mesh_path(mesh_path)
	, m_position(position)
	, m_editor(editor)
{
}


void InsertMeshCommand::serialize(Lumix::JsonSerializer& serializer)
{
	serializer.serialize("path", m_mesh_path.c_str());
	serializer.beginArray("pos");
	serializer.serializeArrayItem(m_position.x);
	serializer.serializeArrayItem(m_position.y);
	serializer.serializeArrayItem(m_position.z);
	serializer.endArray();
}


void InsertMeshCommand::deserialize(Lumix::JsonSerializer& serializer)
{
	char path[Lumix::MAX_PATH_LENGTH];
	serializer.deserialize("path", path, sizeof(path), "");
	m_mesh_path = path;
	serializer.deserializeArrayBegin("pos");
	serializer.deserializeArrayItem(m_position.x, 0);
	serializer.deserializeArrayItem(m_position.y, 0);
	serializer.deserializeArrayItem(m_position.z, 0);
	serializer.deserializeArrayEnd();
}


bool InsertMeshCommand::execute()
{
	Lumix::Universe* universe = m_editor.getUniverse();
	m_entity = universe->createEntity(Lumix::Vec3(0, 0, 0), Lumix::Quat(0, 0, 0, 1));
	universe->setPosition(m_entity, m_position);
	const Lumix::Array<Lumix::IScene*>& scenes = m_editor.getScenes();
	Lumix::ComponentIndex cmp = -1;
	Lumix::IScene* scene = nullptr;
	for (int i = 0; i < scenes.size(); ++i)
	{
		cmp = scenes[i]->createComponent(RENDERABLE_HASH, m_entity);

		if (cmp >= 0)
		{
			scene = scenes[i];
			break;
		}
	}
	if (cmp >= 0)
	{
		char rel_path[Lumix::MAX_PATH_LENGTH];
		m_editor.getRelativePath(rel_path, Lumix::MAX_PATH_LENGTH, m_mesh_path.c_str());
		static_cast<Lumix::RenderScene*>(scene)->setRenderablePath(cmp, rel_path);
	}
	return true;
}


void InsertMeshCommand::undo()
{
	const Lumix::WorldEditor::ComponentList& cmps =
		m_editor.getComponents(m_entity);
	for (int i = 0; i < cmps.size(); ++i)
	{
		cmps[i].scene->destroyComponent(cmps[i].index, cmps[i].type);
	}
	m_editor.getUniverse()->destroyEntity(m_entity);
	m_entity = Lumix::INVALID_ENTITY;
}


Lumix::uint32 InsertMeshCommand::getType()
{
	static const Lumix::uint32 type = Lumix::crc32("insert_mesh");
	return type;
}


bool InsertMeshCommand::merge(IEditorCommand&)
{
	return false;
}


static void insertInScene(Lumix::WorldEditor& editor, Lumix::Model* model)
{
	auto* command = LUMIX_NEW(editor.getAllocator(), InsertMeshCommand)(
		editor, editor.getCameraRaycastHit(), model->getPath());

	editor.executeCommand(command);
}


void AssetBrowser::onGUIModel()
{
	auto* model = static_cast<Lumix::Model*>(m_selected_resource);
	if (m_gui->button("Insert in scene"))
	{
		insertInScene(m_editor, model);
	}

	ImGui::LabelText("Bone count", "%d", model->getBoneCount());
	if (model->getBoneCount() > 0 && m_gui->collapsingHeader("Bones"))
	{
		for (int i = 0; i < model->getBoneCount(); ++i)
		{
			m_gui->text(model->getBone(i).name.c_str());
		}
	}

	ImGui::LabelText("Bounding radius", "%f", model->getBoundingRadius());
	
	auto& lods = model->getLODs();
	if (!lods.empty())
	{
		m_gui->separator();
		m_gui->columns(3);
		m_gui->text("LOD"); m_gui->nextColumn();
		m_gui->text("Distance"); m_gui->nextColumn();
		m_gui->text("# of meshes"); m_gui->nextColumn();
		m_gui->separator();
		for (int i = 0; i < lods.size() - 1; ++i)
		{
			m_gui->text("%d", i); m_gui->nextColumn();
			m_gui->dragFloat("", &lods[i].m_distance); m_gui->nextColumn();
			m_gui->text("%d", lods[i].m_to_mesh - lods[i].m_from_mesh + 1); m_gui->nextColumn();
		}

		m_gui->text("%d", lods.size() - 1); m_gui->nextColumn();
		m_gui->text("INFINITE"); m_gui->nextColumn();
		m_gui->text("%d", lods.back().m_to_mesh - lods.back().m_from_mesh + 1);
		m_gui->columns(1);
	}

	m_gui->separator();
	for (int i = 0; i < model->getMeshCount(); ++i)
	{
		auto& mesh = model->getMesh(i);
		if (ImGui::TreeNode(&mesh, mesh.getName()[0] != 0 ? mesh.getName() : "N/A"))
		{
			ImGui::LabelText("Triangle count", "%d", mesh.getTriangleCount());
			ImGui::LabelText("Material", mesh.getMaterial()->getPath().c_str());
			m_gui->sameLine();
			if (m_gui->button("->"))
			{
				selectResource(mesh.getMaterial()->getPath());
			}
			ImGui::TreePop();
		}
	}
}


void AssetBrowser::onGUIResource()
{
	if (!m_selected_resource) return;

	const char* path = m_selected_resource->getPath().c_str();
	m_gui->separator();
	ImGui::LabelText("Selected resource", path);
	if (!m_history.empty() && m_gui->button("Back"))
	{
		selectResource(m_history.back());
		m_history.pop();
		m_history.pop();
		return;
	}
	m_gui->separator();

	if (!m_selected_resource->isReady() && !m_selected_resource->isFailure())
	{
		m_gui->text("Not ready");
		return;
	}

	char source[Lumix::MAX_PATH_LENGTH];
	if (m_metadata.getString(
			m_selected_resource->getPath().getHash(), SOURCE_HASH, source, Lumix::lengthOf(source)))
	{
		ImGui::LabelText("Source", source);
	}

	auto resource_type = getResourceType(path);
	switch (resource_type)
	{
		case Lumix::ResourceManager::MATERIAL: onGUIMaterial(); break;
		case Lumix::ResourceManager::TEXTURE: onGUITexture(); break;
		case Lumix::ResourceManager::MODEL: onGUIModel(); break;
		case Lumix::ResourceManager::SHADER: onGUIShader(); break;
		default:
			if (resource_type == LUA_SCRIPT_HASH)
			{
				onGUILuaScript();
			}
			else if (resource_type == CLIP_HASH)
			{
				onGUIClip();
			}
			else if (resource_type != UNIVERSE_HASH)
			{
				ASSERT(false); // unimplemented resource
			}
			break;
	}
}


const Lumix::Array<Lumix::Path>& AssetBrowser::getResources(Type type) const
{
	return m_resources[type];
}


void AssetBrowser::addResource(const char* path, const char* filename)
{
	char ext[10];
	Lumix::PathUtils::getExtension(ext, sizeof(ext), filename);

	char fullpath[Lumix::MAX_PATH_LENGTH];
	Lumix::copyString(fullpath, path);
	Lumix::catString(fullpath, "/");
	Lumix::catString(fullpath, filename);

	int index = -1;
	if (Lumix::compareString(ext, "dds") == 0) index = TEXTURE;
	else if (Lumix::compareString(ext, "tga") == 0) index = TEXTURE;
	else if (Lumix::compareString(ext, "raw") == 0) index = TEXTURE;
	else if (Lumix::compareString(ext, "ogg") == 0) index = AUDIO;
	else if (Lumix::compareString(ext, "msh") == 0) index = MODEL;
	else if (Lumix::compareString(ext, "mat") == 0) index = MATERIAL;
	else if (Lumix::compareString(ext, "unv") == 0) index = UNIVERSE;
	else if (Lumix::compareString(ext, "shd") == 0) index = SHADER;
	else if (Lumix::compareString(ext, "lua") == 0) index = LUA_SCRIPT;

	if (Lumix::startsWith(path, "./render_tests") != 0 || Lumix::startsWith(path, "./unit_tests") != 0)
	{
		return;
	}

	if (index >= 0)
	{
		Lumix::Path path_obj(fullpath);
		if (m_resources[index].indexOf(path_obj) == -1)
		{
			m_resources[index].push(path_obj);
		}
	}
}


void AssetBrowser::processDir(const char* dir)
{
	auto* iter = PlatformInterface::createFileIterator(dir, m_editor.getAllocator());
	PlatformInterface::FileInfo info;
	while (getNextFile(iter, &info))
	{
		if (info.filename[0] == '.') continue;

		if (info.is_directory)
		{
			char child_path[Lumix::MAX_PATH_LENGTH];
			Lumix::copyString(child_path, dir);
			Lumix::catString(child_path, "/");
			Lumix::catString(child_path, info.filename);
			processDir(child_path);
		}
		else
		{
			addResource(dir, info.filename);
		}
	}

	destroyFileIterator(iter);
}


void AssetBrowser::findResources()
{
	for (auto& resources : m_resources)
	{
		resources.clear();
	}

	processDir(".");
}