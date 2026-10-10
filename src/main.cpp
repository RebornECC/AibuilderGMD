// Autobuildersai - select objects in the GD editor, type a prompt, an AI decorates them.
// Each level has its own memory (saved per level name + id).
#include <Geode/Geode.hpp>
#include <Geode/modify/EditorUI.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/utils/async.hpp>
#include <matjson.hpp>

using namespace geode::prelude;

namespace {
    std::string memKey(LevelEditorLayer* lel) {
        auto* lvl = lel->m_level;
        return fmt::format("mem_{}_{}", lvl->m_levelID.value(), std::string(lvl->m_levelName));
    }

    std::vector<matjson::Value> toVec(matjson::Value const& v) {
        std::vector<matjson::Value> out;
        if (auto r = v.asArray()) {
            for (auto const& x : *r) out.push_back(x);
        }
        return out;
    }

    matjson::Value loadMemory(std::string const& key) {
        auto s = Mod::get()->getSavedValue<std::string>(key, "[]");
        auto r = matjson::parse(s);
        if (r.isOk() && r.unwrap().isArray()) return r.unwrap();
        return matjson::Value::array();
    }

    void saveMemory(std::string const& key, matjson::Value mem) {
        int keep = (int)Mod::get()->getSettingValue<int64_t>("memory-turns") * 2;
        auto arr = toVec(mem);
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

class AbPopup : public geode::Popup {
protected:
    Ref<LevelEditorLayer> m_lel;
    Ref<EditorUI> m_ui;
    TextInput* m_input = nullptr;
    CCLabelBMFont* m_status = nullptr;

    void say(std::string const& text) {
        m_status->setString(text.c_str());
        m_status->setScale(.55f);
        float maxW = m_mainLayer->getContentSize().width - 30.f;
        float w = m_status->getContentSize().width * m_status->getScale();
        if (w > maxW) m_status->setScale(.55f * maxW / w);
    }

    bool init(LevelEditorLayer* lel, EditorUI* ui) {
        if (!Popup::init(440.f, 240.f)) return false;
        m_lel = lel;
        m_ui = ui;
        this->setTitle("Autobuildersai");
        auto size = m_mainLayer->getContentSize();

        m_input = TextInput::create(size.width - 40.f, "Describe the decor...", "chatFont.fnt");
        m_input->setPosition({size.width / 2, size.height - 80.f});
        m_mainLayer->addChild(m_input);

        m_status = CCLabelBMFont::create("Select objects, then press Generate.", "chatFont.fnt");
                m_status->setPosition({size.width / 2, 95.f});
        m_mainLayer->addChild(m_status);
        this->say("Select objects, then press Generate.");

        auto menu = CCMenu::create();
        menu->setPosition({0, 0});
        m_mainLayer->addChild(menu);

        auto gen = CCMenuItemExt::createSpriteExtra(ButtonSprite::create("Generate"), [this](auto) { this->generate(); });
        gen->setScale(.75f);
        gen->setPosition({size.width / 2 - 100.f, 40.f});
        menu->addChild(gen);

        auto clr = CCMenuItemExt::createSpriteExtra(ButtonSprite::create("Clear memory"), [this](auto) {
            Mod::get()->setSavedValue<std::string>(memKey(m_lel), "[]");
            this->say("Memory of this level cleared.");
        });
        clr->setScale(.75f);
        clr->setPosition({size.width / 2 + 100.f, 40.f});
        menu->addChild(clr);
        return true;
    }

    void generate() {
        auto prompt = std::string(m_input->getString());
        if (prompt.empty()) { this->say("Type a prompt first."); return; }
        auto provider = Mod::get()->getSettingValue<std::string>("provider");
        bool gemini = provider == "gemini";
        bool deepseek = provider == "deepseek";
        auto key = Mod::get()->getSettingValue<std::string>(gemini ? "gemini-key" : deepseek ? "deepseek-key" : "api-key");
        if (key.empty()) { this->say("Set the API key for the chosen provider in the mod settings."); return; }

        auto* sel = m_ui->getSelectedObjects();
        if (!sel || sel->count() == 0) { this->say("Select objects first."); return; }

        std::string selection;
        for (auto* o : CCArrayExt<GameObject*>(sel)) {
            selection += std::string(o->getSaveString(m_lel)) + ";";
            if (selection.size() > 20000) break;
        }

        auto mkey = memKey(m_lel);
        auto mem = loadMemory(mkey);

        matjson::Value cur;
        cur["role"] = "user";
        cur["content"] = fmt::format("Selected objects:\n{}\n\nPrompt: {}", selection, prompt);

        matjson::Value body;
        std::string url;
        web::WebRequest req;
        if (gemini) {
            matjson::Value contents = matjson::Value::array();
            auto addTurn = [&](matjson::Value const& m) {
                matjson::Value part;
                part["text"] = m["content"].asString().unwrapOr("");
                matjson::Value parts = matjson::Value::array();
                parts.push(part);
                matjson::Value c;
                c["role"] = m["role"].asString().unwrapOr("user") == "assistant" ? "model" : "user";
                c["parts"] = parts;
                contents.push(c);
            };
            for (auto const& v : toVec(mem)) addTurn(v);
            addTurn(cur);
            matjson::Value sysPart;
            sysPart["text"] = SYSTEM;
            matjson::Value sysParts = matjson::Value::array();
            sysParts.push(sysPart);
            matjson::Value sysInstr;
            sysInstr["parts"] = sysParts;
            body["systemInstruction"] = sysInstr;
            body["contents"] = contents;
            url = fmt::format(
                "https://generativelanguage.googleapis.com/v1beta/models/{}:generateContent",
                Mod::get()->getSettingValue<std::string>("gemini-model")
            );
            req.header("x-goog-api-key", key);
        } else if (deepseek) {
            matjson::Value messages = matjson::Value::array();
            matjson::Value sysMsg;
            sysMsg["role"] = "system";
            sysMsg["content"] = SYSTEM;
            messages.push(sysMsg);
            for (auto const& v : toVec(mem)) messages.push(v);
            messages.push(cur);
            body["model"] = Mod::get()->getSettingValue<std::string>("deepseek-model");
            body["max_tokens"] = 8000;
            body["stream"] = false;
            body["messages"] = messages;
            url = "https://api.deepseek.com/chat/completions";
            req.header("Authorization", "Bearer " + key);
        } else {
            matjson::Value messages = matjson::Value::array();
            for (auto const& v : toVec(mem)) messages.push(v);
            messages.push(cur);
            body["model"] = Mod::get()->getSettingValue<std::string>("model");
            body["max_tokens"] = 8000;
            body["system"] = SYSTEM;
            body["messages"] = messages;
            url = "https://api.anthropic.com/v1/messages";
            req.header("x-api-key", key);
            req.header("anthropic-version", "2023-06-01");
        }
        req.header("content-type", "application/json");
        req.bodyJSON(body);

        this->say("Thinking...");

        Ref<LevelEditorLayer> lel = m_lel;
        Ref<AbPopup> self = this;
        async::spawn(
            [req = std::move(req), url]() mutable -> arc::Future<web::WebResponse> {
                co_return co_await req.post(url);
            },
            [lel, self, mkey, prompt, cur, gemini, deepseek](web::WebResponse res) {
                auto status = [self](std::string s) { self->say(s); };
                if (!res.ok()) {
                    status(fmt::format("API error {}", res.code()));
                    return;
                }
                auto json = res.json();
                if (!json.isOk()) { status("Bad response."); return; }
                std::string text = gemini
                    ? (*json)["candidates"][0]["content"]["parts"][0]["text"].asString().unwrapOr("")
                    : deepseek
                    ? (*json)["choices"][0]["message"]["content"].asString().unwrapOr("")
                    : (*json)["content"][0]["text"].asString().unwrapOr("");
                auto objs = sanitize(stripFences(text), (size_t)Mod::get()->getSettingValue<int64_t>("max-objects"));
                if (objs.empty()) { status("AI returned no objects."); return; }
                auto* made = lel->createObjectsFromString(objs, false, true);
                size_t n = made ? made->count() : 0;

                auto mem2 = loadMemory(mkey);
                mem2.push(cur);
                matjson::Value reply;
                reply["role"] = "assistant";
                reply["content"] = fmt::format("(added {} decor objects for: {})", n, prompt);
                mem2.push(reply);
                saveMemory(mkey, mem2);
                status(fmt::format("Added {} objects. Undo works as usual.", n));
            }
        );
    }

public:
    static AbPopup* create(LevelEditorLayer* lel, EditorUI* ui) {
        auto ret = new AbPopup();
        if (ret->init(lel, ui)) {
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
