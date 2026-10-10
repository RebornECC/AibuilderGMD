// Autobuildersai - select objects in the GD editor, type a prompt, an AI decorates them.
// Each level has its own memory (saved per level name + id).
#include <Geode/Geode.hpp>
#include <Geode/modify/EditorUI.hpp>
#include <Geode/utils/web.hpp>
#include <matjson.hpp>

using namespace geode::prelude;

namespace {
    std::string memKey(LevelEditorLayer* lel) {
        auto* lvl = lel->m_level;
        return fmt::format("mem_{}_{}", lvl->m_levelID.value(), std::string(lvl->m_levelName));
    }

    matjson::Value loadMemory(std::string const& key) {
        auto s = Mod::get()->getSavedValue<std::string>(key, "[]");
        auto r = matjson::parse(s);
        if (r.isOk() && r.unwrap().isArray()) return r.unwrap();
        return matjson::Value::array();
    }

    void saveMemory(std::string const& key, matjson::Value mem) {
        int keep = (int)Mod::get()->getSettingValue<int64_t>("memory-turns") * 2;
        auto arr = mem.asArray().unwrapOr(std::vector<matjson::Value>{});
        while ((int)arr.size() > keep) arr.erase(arr.begin());
        matjson::Value out = matjson::Value::array();
        for (auto& v : arr) out.push(v);
        Mod::get()->setSavedValue<std::string>(key, out.dump(matjson::NO_INDENTATION));
    }

    std::string stripFences(std::string s) {
        std::string out;
        size_t pos = 0;
        while (pos < s.size()) {
            auto nl = s.find('\n', pos);
            auto line = s.substr(pos, nl == std::string::npos ? nl : nl - pos);
            if (line.rfind("```", 0) != 0) out += line;
            pos = nl == std::string::npos ? s.size() : nl + 1;
        }
        return out;
    }

    // keep only characters that can appear in a GD object string
    std::string sanitize(std::string s, size_t maxObjects) {
        std::string out;
        size_t objects = 0;
        for (char c : s) {
            if (c == ';') {
                if (++objects > maxObjects) break;
                out += c;
            } else if ((c >= '0' && c <= '9') || c == ',' || c == '.' || c == '-' || c == '_' || c == '|' || c == '=') {
                out += c;
            }
        }
        return out;
    }

    char const* SYSTEM = R"(You are Autobuildersai, a Geometry Dash level decorator running inside the level editor.
The user selects objects and gives a prompt. You receive the selected objects as Geometry Dash object save strings
(comma separated key,value pairs; 1=object id, 2=x, 3=y, 6=rotation, 21=main color channel, 22=detail color channel,
24=z layer, 25=z order, 32=scale, 128=scale x, 129=scale y, 57=groups). Objects are separated by ';'.
Reply with ONLY new decoration objects in the same format: one object per entry, each ending with ';'.
No explanations, no markdown, no code fences, no coordinates outside the area around the selection.
Decor must not block gameplay: use decoration/background object ids, keep it away from the player's path
unless the user asks otherwise, and never add triggers unless the user asks for them.
Respect the style and the earlier requests remembered for this level.)";
}

class AbPopup : public geode::Popup<LevelEditorLayer*, EditorUI*> {
protected:
    Ref<LevelEditorLayer> m_lel;
    Ref<EditorUI> m_ui;
    TextInput* m_input = nullptr;
    CCLabelBMFont* m_status = nullptr;

    bool setup(LevelEditorLayer* lel, EditorUI* ui) override {
        m_lel = lel;
        m_ui = ui;
        this->setTitle("Autobuildersai");
        auto size = m_mainLayer->getContentSize();

        m_input = TextInput::create(size.width - 40.f, "Describe the decor (e.g. neon city, dark forest)...", "chatFont.fnt");
        m_input->setPosition({size.width / 2, size.height - 70.f});
        m_mainLayer->addChild(m_input);

        m_status = CCLabelBMFont::create("Select objects, then press Generate.", "chatFont.fnt");
        m_status->setScale(.6f);
        m_status->setPosition({size.width / 2, size.height / 2 - 5.f});
        m_mainLayer->addChild(m_status);

        auto menu = CCMenu::create();
        menu->setPosition({0, 0});
        m_mainLayer->addChild(menu);

        auto gen = CCMenuItemExt::createSpriteExtra(ButtonSprite::create("Generate"), [this](auto) { this->generate(); });
        gen->setPosition({size.width / 2 - 70.f, 40.f});
        menu->addChild(gen);

        auto clr = CCMenuItemExt::createSpriteExtra(ButtonSprite::create("Clear memory"), [this](auto) {
            Mod::get()->setSavedValue<std::string>(memKey(m_lel), "[]");
            m_status->setString("Memory of this level cleared.");
        });
        clr->setPosition({size.width / 2 + 70.f, 40.f});
        menu->addChild(clr);
        return true;
    }

    void generate() {
        auto prompt = std::string(m_input->getString());
        if (prompt.empty()) { m_status->setString("Type a prompt first."); return; }
        auto key = Mod::get()->getSettingValue<std::string>("api-key");
        if (key.empty()) { m_status->setString("Set your API key in the mod settings."); return; }

        auto* sel = m_ui->getSelectedObjects();
        if (!sel || sel->count() == 0) { m_status->setString("Select objects first."); return; }

        std::string selection;
        for (auto* o : CCArrayExt<GameObject*>(sel)) {
            selection += std::string(o->getSaveString(m_lel)) + ";";
            if (selection.size() > 20000) break;
        }

        auto mkey = memKey(m_lel);
        auto mem = loadMemory(mkey);

        matjson::Value messages = matjson::Value::array();
        for (auto& v : mem.asArray().unwrapOr(std::vector<matjson::Value>{})) messages.push(v);
        matjson::Value cur;
        cur["role"] = "user";
        cur["content"] = fmt::format("Selected objects:\n{}\n\nPrompt: {}", selection, prompt);
        messages.push(cur);

        matjson::Value body;
        body["model"] = Mod::get()->getSettingValue<std::string>("model");
        body["max_tokens"] = 8000;
        body["system"] = SYSTEM;
        body["messages"] = messages;

        auto req = web::WebRequest();
        req.header("x-api-key", key);
        req.header("anthropic-version", "2023-06-01");
        req.header("content-type", "application/json");
        req.bodyJSON(body);

        m_status->setString("Thinking...");

        static EventListener<web::WebTask>* listener = new EventListener<web::WebTask>();
        Ref<LevelEditorLayer> lel = m_lel;
        WeakRef<AbPopup> self = this;
        listener->bind([lel, self, mkey, prompt, cur](web::WebTask::Event* e) {
            auto* res = e->getValue();
            if (!res) return;
            auto status = [self](std::string s) {
                if (auto p = self.lock()) p->m_status->setString(s.c_str());
                else Notification::create(s, NotificationIcon::Info)->show();
            };
            if (!res->ok()) {
                status(fmt::format("API error {}", res->code()));
                return;
            }
            auto json = res->json();
            if (!json.isOk()) { status("Bad response."); return; }
            auto text = (*json)["content"][0]["text"].asString().unwrapOr("");
            auto objs = sanitize(stripFences(text), (size_t)Mod::get()->getSettingValue<int64_t>("max-objects"));
            if (objs.empty()) { status("AI returned no objects."); return; }
            auto* made = lel->createObjectsFromString(objs, false, true);
            size_t n = made ? made->count() : 0;

            auto mem = loadMemory(mkey);
            mem.push(cur);
            matjson::Value reply;
            reply["role"] = "assistant";
            reply["content"] = fmt::format("(added {} decor objects for: {})", n, prompt);
            mem.push(reply);
            saveMemory(mkey, mem);
            status(fmt::format("Added {} objects. Undo works as usual.", n));
        });
        listener->setFilter(req.post("https://api.anthropic.com/v1/messages"));
    }

public:
    static AbPopup* create(LevelEditorLayer* lel, EditorUI* ui) {
        auto ret = new AbPopup();
        if (ret->initAnchored(380.f, 220.f, lel, ui)) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

class $modify(AbEditorUI, EditorUI) {
    bool init(LevelEditorLayer* lel) {
        if (!EditorUI::init(lel)) return false;
        auto menu = CCMenu::create();
        menu->setID("autobuildersai-menu"_spr);
        auto spr = ButtonSprite::create("AI", "bigFont.fnt", "GJ_button_04.png", .6f);
        auto btn = CCMenuItemExt::createSpriteExtra(spr, [this, lel](auto) {
            AbPopup::create(lel, this)->show();
        });
        menu->addChild(btn);
        auto win = CCDirector::get()->getWinSize();
        menu->setPosition({win.width - 30.f, win.height - 60.f});
        this->addChild(menu, 100);
        return true;
    }
};
