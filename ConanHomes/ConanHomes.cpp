// ============================================================================
//  ConanHomes — saved locations a player can teleport back to.
//
//  WHAT IT IS
//  ----------
//  A player stands somewhere, names it, and can come back to it later from
//  anywhere on the map. Their mount comes along, and so do their followers —
//  arriving home without the thrall you spent an hour levelling is the kind of
//  detail that turns a nice feature into a complaint.
//
//      !casa add <name>     save this spot
//      !casa <name>         go there
//      !casa del <name>     forget it
//      !homes               list them
//
//  Every command name and every message lives in config.json. The source is
//  English; the text players read is data.
//
//  HOW THE TELEPORT WORKS
//  ----------------------
//      ConanPlayerController::TeleportPlayerServer(loc, rot, RunCheatCheck,
//                                                  SnapToGround, ...)
//
//  This is the game's own path — the same one the server takes when a player
//  logs back in. It is NOT the admin one: `AdminTeleportPlayerServer` exists
//  separately, which is what says this one is for ordinary players.
//
//  `SnapToGround` is the game solving, for free, the problem every teleport
//  plugin gets wrong on its first day: a saved Z that is now inside a rock.
//
//  MOUNT AND FOLLOWERS
//  -------------------
//      ConanCharacter::IsRiding() / GetMount()          the mount
//      ConanCharacter::GetMyFormationLeaderComponent()  the followers
//        FormationLeaderComponent::GetMaxFollowerSlots()
//        FormationLeaderComponent::GetFollower(slot)
//
//  Each of those is an actor and moves with `Actor::K2_SetActorLocation`. They
//  are moved AFTER the player, and every one is counted — the log says how many
//  came along, not how many were asked to.
//
//  THE WAIT, AND WHY IT CAN BE CANCELLED
//  -------------------------------------
//  A teleport that fires instantly is an escape button: take a hit, vanish.
//  So `!casa` starts a countdown, and the countdown dies if the player takes
//  damage or dies during it. Both are configurable, because a private building
//  server wants none of it and a PvP server wants all of it.
//
//  Combat is tracked the way the game already tracks it: `HandleTakeDamage` is
//  hooked, the damaged actor's owner is resolved, and a timestamp is kept. That
//  covers taking damage AND dealing it — a player who just hit somebody is in
//  combat even if nothing touched them back.
//
//  POINTS
//  ------
//  The costs are in config.json and default to zero. Conan-Api has no
//  inter-plugin points API yet — ConanShop keeps its points to itself — so
//  nothing is charged today. The fields exist so that turning charging on later
//  is editing a config, not editing this file.
//
//  BUILD
//     ./compilar.sh          (Linux/WSL)      compilar.bat  (Windows)
//  INSTALL
//     copy the folder into <server>/ConanSandbox/Binaries/Win64/Conan-Api/Plugins/
//
//  The folder has a HYPHEN: `Conan-Api`, never run together.
// ============================================================================
#include "Conan/ConanPluginApi.h"
#include "Conan/ConanBase.h"
#include "Conan/ConanPermission.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <ctime>
#include <string>
#include <vector>
#include <map>

static const ConanApiTabela* g_api = nullptr;

// ── the game's vector ───────────────────────────────────────────────────────
//
// 24 bytes, three doubles. Checked against this build's reflection rather than
// assumed: UE5 widened FVector from float to double, and a plugin that still
// believes in 12 bytes writes garbage into two thirds of a position with no
// error anywhere.
struct FVec { double x = 0, y = 0, z = 0; };
struct FRot { double pitch = 0, yaw = 0, roll = 0; };

// ── offsets inside ChatRpcData ──────────────────────────────────────────────
static const uint32_t CHAT_TEXT = 0x068;   // FString Message

// ── what the server owner can change ────────────────────────────────────────
static std::string g_cmdRoot   = "!home";    // "!home add x" · "!home x" · "!home del x"
static std::string g_cmdList   = "!homes";
static std::string g_subAdd    = "add";
static std::string g_subDel    = "del";
static std::string g_permission = "";

static int  g_maxHomes    = 5;
static int  g_waitSeconds = 10;   // countdown before the teleport fires
static int  g_cooldown    = 60;   // between teleports
static bool g_takeMount   = true;
static bool g_takeFollowers = true;
static bool g_cancelOnDamage = true;
static bool g_blockInCombat  = true;
static int  g_combatSeconds  = 10;

// Costs live in the config and are read, but nothing is charged: there is no
// points API between plugins yet. Kept so the day there is one, it is a config
// change and not a code change.
static int g_costAdd = 0, g_costGo = 0, g_costDel = 0;

static std::map<std::string, std::string> g_msg;

static const char* Msg(const char* chave, const char* padrao)
{
    std::map<std::string, std::string>::const_iterator it = g_msg.find(chave);
    return (it == g_msg.end() || it->second.empty()) ? padrao : it->second.c_str();
}

// ── {marks} inside a message ────────────────────────────────────────────────
//
// A plain search-and-replace, never printf: the text comes from a file the
// server owner edits, and a stray `%s` in somebody's translation would crash
// the game thread.
static std::string Fill(const std::string& texto,
                        const char* m1 = nullptr, const std::string& v1 = "",
                        const char* m2 = nullptr, const std::string& v2 = "",
                        const char* m3 = nullptr, const std::string& v3 = "")
{
    std::string s = texto;
    const char* marks[3] = { m1, m2, m3 };
    const std::string* vals[3] = { &v1, &v2, &v3 };
    for (int i = 0; i < 3; ++i)
    {
        if (!marks[i]) continue;
        const size_t n = std::strlen(marks[i]);
        for (size_t p = s.find(marks[i]); p != std::string::npos;
             p = s.find(marks[i], p + vals[i]->size()))
            s = s.substr(0, p) + *vals[i] + s.substr(p + n);
    }
    return s;
}

static std::string Num(long v) { char b[24]; std::snprintf(b, sizeof(b), "%ld", v); return b; }

// ── one saved place ─────────────────────────────────────────────────────────
struct Home
{
    std::string name;
    FVec        where;
    double      yaw = 0;
};

struct Player
{
    std::string          displayName;
    std::vector<Home>    homes;
};

static std::map<std::string, Player> g_data;      // key = the player's stable id

// ── a teleport that has been asked for but has not happened yet ─────────────
struct Pending
{
    std::string name;
    FVec        target;
    double      firesAt = 0;     // when it fires
    float       healthAtStart = 0;
};
static std::map<std::string, Pending> g_pending;
static std::map<std::string, double>   g_lastTrip;    // last teleport, per player
static std::map<void*, double>         g_combat;    // last damage, per character

// Wall clock, in seconds. NOT `clock()`: that one measures CPU time, and on a
// server under load a cooldown built on it would drift away from the seconds
// the player is actually counting. Second resolution is plenty — everything
// here is measured in whole seconds by design.
static double Now()
{
    return double(std::time(nullptr));
}

// ── reaching things by name ─────────────────────────────────────────────────
//
// Every offset here is resolved through reflection on whichever build is
// running. A number baked into the plugin works today and reads the
// neighbouring field after Funcom's next patch — with no error and no log line,
// just wrong data.
static void* PointerMember(void* obj, const char* name)
{
    if (!obj || !g_api) return nullptr;
    const int32_t off = g_api->OffsetDoMembro(obj, name);
    if (off < 0) return nullptr;
    void* p = nullptr;
    if (g_api->LerMembro(obj, uint32_t(off), &p, sizeof(p)) <= 0) return nullptr;
    if (!p || !g_api->Legivel(p, 8)) return nullptr;
    return p;
}

static void* CharacterOf(void* controller)
{
    if (void* p = PointerMember(controller, "Pawn"))             return p;
    if (void* p = PointerMember(controller, "Character"))        return p;
    if (void* p = PointerMember(controller, "AcknowledgedPawn")) return p;
    return nullptr;
}

// Where an actor is standing. Returns false when the game did not answer, so
// the caller never saves a position of (0,0,0) as if it were a place.
static bool ReadPosition(void* actor, FVec& saida)
{
    if (!actor) return false;
    saida = ConanApi::Call<FVec>(actor, "K2_GetActorLocation");
    return g_api->UltimaChamadaExecutou() != 0;
}

// ── moving one actor ────────────────────────────────────────────────────────
//
// For the player this is the wrong tool: `TeleportPlayerServer` below does the
// networking and the ground snap. For a mount or a follower, which are just
// actors, this is the whole job.
// `FHitResult` is 256 bytes on this build — checked against reflection, not
// guessed, because the API refuses a size mismatch and passing the wrong one
// would either overrun the parameter block or leave part of it uninitialised.
struct HitBruto { unsigned char bytes[256]; };

static bool MoveActor(void* actor, const FVec& target)
{
    if (!actor) return false;
    FVec     d = target;
    HitBruto hit;                          // output we never read
    std::memset(&hit, 0, sizeof(hit));
    // BOTH answers are needed, and the first version of this used only the
    // wrong one.
    //
    // `UltimaChamadaExecutou()` says the function was DISPATCHED.
    // `K2_SetActorLocation` RETURNS whether the actor actually moved.
    //
    // Checking only dispatch is how this project spent a morning telling
    // players their knowledge was unlocked while the game had quietly refused.
    // The same mistake here would report a mount that stayed behind.
    const bool moveu = ConanApi::CallSaida<bool>(actor, "K2_SetActorLocation",
                                                 d, bool(false),
                                                 ConanApi::ParaFora(hit),
                                                 bool(true));   // bTeleport
    return g_api->UltimaChamadaExecutou() != 0 && moveu;
}

// ── the player ──────────────────────────────────────────────────────────────
//
// RunCheatCheck=false: this is the server moving its own player at the player's
// request, not a client claiming to have moved. SnapToGround=true is the game
// solving the buried-in-a-rock problem for us.
static bool TeleportPlayer(void* controller, const FVec& target, double yaw)
{
    FVec d = target;
    FRot r; r.yaw = yaw;
    ConanApi::CallSaida<void>(controller, "TeleportPlayerServer",
                              ConanApi::ParaEntreSai(d),
                              ConanApi::ParaEntreSai(r),
                              bool(false),   // RunCheatCheck
                              bool(true),    // SnapToGround
                              bool(true),    // ForceTeleportClients
                              bool(false));  // UsePawnRotationInstead
    if (g_api->UltimaChamadaExecutou() == 0) return false;

    // ── and then MEASURE it ─────────────────────────────────────────────────
    //
    // This function returns void, so there is no result to check — which is
    // exactly the situation where a plugin starts believing its own calls. So
    // the character is asked where it is now, and the distance is logged.
    //
    // The distance is REPORTED, not judged. `SnapToGround` moves the player up
    // or down on purpose, and a teleport finishes streaming over the following
    // frames, so a few metres here mean nothing while a few kilometres mean the
    // teleport did not happen. Printing the number lets the log settle that
    // argument instead of a guess baked in today.
    FVec now;
    void* character = CharacterOf(controller);
    if (character && ReadPosition(character, now))
    {
        const double dx = now.x - target.x;
        const double dy = now.y - target.y;
        const double dz = now.z - target.z;
        const double dist2 = dx*dx + dy*dy + dz*dz;
        if (dist2 > 500.0 * 500.0)
            g_api->Log("[homes] WARNING: asked for (%.0f %.0f %.0f), the character "
                       "reads (%.0f %.0f %.0f) right after — %.0f units away. The "
                       "call was accepted; the move may still be streaming, or it "
                       "may not have happened.",
                       target.x, target.y, target.z,
                       now.x, now.y, now.z, dist2 > 0 ? std::sqrt(dist2) : 0.0);
    }
    return true;
}

// ── the mount ───────────────────────────────────────────────────────────────
static int BringMount(void* character, const FVec& target)
{
    if (!g_takeMount || !character) return 0;

    const bool montado = ConanApi::Call<bool>(character, "IsRiding");
    if (!g_api->UltimaChamadaExecutou() || !montado) return 0;

    void* mount = ConanApi::Call<void*>(character, "GetMount");
    if (!g_api->UltimaChamadaExecutou() || !mount || !g_api->Legivel(mount, 8)) return 0;

    return MoveActor(mount, target) ? 1 : 0;
}

// ── the followers ───────────────────────────────────────────────────────────
//
// THREE ROUTES, TRIED IN ORDER, AND THE LOG SAYS WHICH ONE ANSWERED.
//
// The first version had only the formation component, and the first live test
// said `0 follower(s)` with a pet visibly walking behind the player. Worse, it
// said it silently. The instrumented version then produced the finding that
// shaped this code:
//
//     followers: no formation component — BUT HasAnyFollowingFollowers said yes
//
// The game confirmed the player HAD a follower and the component was null. One
// route was never going to be enough, and picking the next one by guessing
// would have cost another restart and another test.
//
// So: ask the game whether there is anything to find, then try each route and
// stop at the first that produces actors. Every outcome is logged, including
// the routes that were skipped because an earlier one worked.
static const int MAX_FOLLOWERS = 64;

// Route 1 — the formation component on the character.
static int ByFormation(void* character, void** saida)
{
    void* formation = ConanApi::Call<void*>(character, "GetMyFormationLeaderComponent");
    if (!g_api->UltimaChamadaExecutou() || !formation || !g_api->Legivel(formation, 8))
        return -1;                                     // route unavailable

    int32_t slots = ConanApi::Call<int32_t>(formation, "GetMaxFollowerSlots");
    if (!g_api->UltimaChamadaExecutou() || slots <= 0) return -1;
    if (slots > MAX_FOLLOWERS) slots = MAX_FOLLOWERS;

    int n = 0;
    for (int32_t i = 0; i < slots; ++i)
    {
        void* f = ConanApi::Call<void*>(formation, "GetFollower", int32_t(i));
        if (!g_api->UltimaChamadaExecutou() || !f || !g_api->Legivel(f, 8)) continue;
        saida[n++] = f;
    }
    return n;
}

// Route 2 — the game's own library, which answers with the characters directly.
//
// `GetCombatThrallsFromPlayer` is a Blueprint function library, so the call goes
// to its class default object. The nameplate over a following pet reads
// "[Warrior]", which is what suggested this one: a pet set to follow IS a combat
// thrall as far as the game is concerned.
static int ByLibrary(void* character, void** saida)
{
    void* lib = g_api->GetDefaultObject("FuncomFunctionLibrary_C");
    if (!lib) return -1;

    int n = 0;
    ConanApi::CallSaida<void>(lib, "GetCombatThrallsFromPlayer",
                              character, character,   // Player, __WorldContext
                              ConanApi::ParaForaLista(saida, MAX_FOLLOWERS, n));
    if (!g_api->UltimaChamadaExecutou()) return -1;
    return n;
}

// Route 3 — sweep the world's thrall components and ask each one whose it is.
//
// The most expensive and the most certain: `IsOwner` is the game's own ownership
// check, so there is no guessing about who belongs to whom. Last on purpose —
// on a busy server this walks every follower of every player.
static int BySweep(void* controller, void** saida)
{
    static void* comps[4096];
    const int total = g_api->FindObjects("ThrallComponent", comps, 4096, /*subclasses=*/1);
    if (total <= 0) return -1;

    int n = 0;
    for (int i = 0; i < total && n < MAX_FOLLOWERS; ++i)
    {
        if (!comps[i]) continue;
        char name[128] = {0};
        if (g_api->NomeDoObjeto(comps[i], name, sizeof(name)) &&
            std::strncmp(name, "Default__", 9) == 0) continue;

        const bool meu = ConanApi::Call<bool>(comps[i], "IsOwner", controller);
        if (!g_api->UltimaChamadaExecutou() || !meu) continue;

        void* dono = ConanApi::Call<void*>(comps[i], "GetOwner");
        if (!g_api->UltimaChamadaExecutou() || !dono || !g_api->Legivel(dono, 8)) continue;
        saida[n++] = dono;
    }
    g_api->Log("[homes] followers: swept %d thrall component(s), %d belong to this player.",
               total, n);
    return n;
}

static int BringFollowers(void* controller, void* character, const FVec& target)
{
    if (!g_takeFollowers || !character) return 0;

    // The game's own answer first. It is the cross-check that turns "found
    // nothing" into either "there was nothing" or "the route is broken".
    const bool hasAny  = ConanApi::Call<bool>(controller, "HasAnyFollowingFollowers");
    const bool asked = g_api->UltimaChamadaExecutou() != 0;

    void* found[MAX_FOLLOWERS];
    int n = 0;
    const char* route = "(none)";

    n = ByFormation(character, found);              if (n > 0) route = "formation";
    if (n <= 0) { n = ByLibrary(character, found); if (n > 0) route = "library"; }
    if (n <= 0) { n = BySweep(controller, found);  if (n > 0) route = "sweep";   }

    if (n <= 0)
    {
        g_api->Log("[homes] followers: none found by any of the three routes "
                   "(game says has-followers=%s).",
                   asked ? (hasAny ? "YES — so this is a defect" : "no") : "not answered");
        return 0;
    }

    int moved = 0;
    for (int i = 0; i < n; ++i)
        if (MoveActor(found[i], target)) ++moved;

    g_api->Log("[homes] followers: route \"%s\" · %d found · %d moved "
               "(game says has-followers=%s)",
               route, n, moved,
               asked ? (hasAny ? "yes" : "no") : "not answered");
    return moved;
}

// ── answering the player ────────────────────────────────────────────────────
//
// Straight at the controller. Conan Shop learned the other way round the
// expensive way: its first version looked the player up by name to deliver text
// to somebody it already had in hand, and answered nothing at all.
static void Reply(void* controller, const std::string& texto)
{
    if (!controller || texto.empty() || !g_api) return;

    static int route = 0;
    for (int tentativa = 0; tentativa < 2; ++tentativa)
    {
        const int qual = (route > 0 && tentativa == 0) ? route : (tentativa + 1);
        if (qual == 1)
        {
            ConanApi::Call<void>(controller, "ClientHUDShowNotification",
                                 ConanApi::TextoRico(texto.c_str()),
                                 bool(true), bool(false));
        }
        else
        {
            ConanApi::Call<void>(controller, "ClientMessage",
                                 ConanApi::Texto(texto.c_str()),
                                 ConanApi::Nome("Event"), float(8.0f));
        }
        if (g_api->UltimaChamadaExecutou())
        {
            if (route != qual)
            {
                route = qual;
                g_api->Log("[homes] talking to the player through %s",
                           qual == 1 ? "ClientHUDShowNotification" : "ClientMessage");
            }
            return;
        }
    }
}

// ── who is this ─────────────────────────────────────────────────────────────
//
// The stable account id. A character NAME is not an acceptable substitute:
// players rename, and two people can share a name — a homes file keyed on it
// would eventually hand somebody another player's houses.
static std::string PlayerId(void* controller)
{
    // Permission first when it is installed. It already answers this, and it is
    // the same answer every other plugin on the server is keying on.
    char buf[CONAN_PERM_MAX_ID] = {0};
    if (ConanPermIdDoController(controller, buf, sizeof(buf)) > 0 && buf[0]) return buf;

    // And the same key read directly when it is not. `MasterAccountId` on the
    // PlayerState is exactly what Permission reads, so reading it here turns a
    // hard dependency into an optional one: this plugin works on a server that
    // installed nothing else.
    void* state = PointerMember(controller, "PlayerState");
    if (!state) return std::string();

    const int32_t off = g_api->OffsetDoMembro(state, "MasterAccountId");
    if (off < 0) return std::string();

    char id[CONAN_PERM_MAX_ID] = {0};
    if (g_api->LerTextoDoJogo(state, uint32_t(off), id, sizeof(id)) <= 0 || !id[0])
        return std::string();
    return id;
}

// ── saving and loading ──────────────────────────────────────────────────────
//
// One flat file. The shape is deliberately boring — id, name, three numbers —
// because a homes file that a server owner can open, read and fix by hand is
// worth more than a database they cannot.
//
// Written whole and renamed into place: a server that dies mid-write leaves the
// previous file intact rather than a truncated one.
static std::string DataPath()
{
    const char* p = g_api->CaminhoDados("ConanHomes", "homes.txt");
    return p ? std::string(p) : std::string();
}

static void Save()
{
    const std::string goal = DataPath();
    if (goal.empty()) return;
    const std::string tmp = goal + ".novo";

    FILE* f = nullptr;
    CONAN_FOPEN(f, tmp.c_str(), "wb");
    if (!f) { g_api->Log("[homes] could not write %s", tmp.c_str()); return; }

    std::fprintf(f, "# ConanHomes — one home per line\n");
    std::fprintf(f, "# id<TAB>name<TAB>x<TAB>y<TAB>z<TAB>yaw<TAB>player\n");
    int n = 0;
    for (std::map<std::string, Player>::const_iterator it = g_data.begin();
         it != g_data.end(); ++it)
    {
        for (size_t i = 0; i < it->second.homes.size(); ++i)
        {
            const Home& h = it->second.homes[i];
            std::fprintf(f, "%s\t%s\t%.3f\t%.3f\t%.3f\t%.3f\t%s\n",
                         it->first.c_str(), h.name.c_str(),
                         h.where.x, h.where.y, h.where.z, h.yaw,
                         it->second.displayName.c_str());
            ++n;
        }
    }
    std::fclose(f);

    std::remove(goal.c_str());
    if (std::rename(tmp.c_str(), goal.c_str()) != 0)
        g_api->Log("[homes] could not put %s in place", goal.c_str());
    else
        g_api->Log("[homes] saved: %d home(s).", n);
}

static void Load()
{
    const std::string goal = DataPath();
    if (goal.empty()) return;
    FILE* f = nullptr;
    CONAN_FOPEN(f, goal.c_str(), "rb");
    if (!f) { g_api->Log("[homes] no saved homes yet — starting empty."); return; }

    char line[1024];
    int n = 0, bad = 0;
    while (std::fgets(line, sizeof(line), f))
    {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;

        char id[128] = {0}, name[64] = {0}, who[128] = {0};
        double x = 0, y = 0, z = 0, yaw = 0;
        const int lidos = std::sscanf(line, "%127[^\t]\t%63[^\t]\t%lf\t%lf\t%lf\t%lf\t%127[^\n\r]",
                                      id, name, &x, &y, &z, &yaw, who);
        // Six is the minimum: the display name is a convenience and an old file
        // written before it existed must still load.
        if (lidos < 6 || !id[0] || !name[0]) { ++bad; continue; }

        Home h; h.name = name; h.where.x = x; h.where.y = y; h.where.z = z; h.yaw = yaw;
        Player& j = g_data[id];
        if (who[0]) j.displayName = who;
        j.homes.push_back(h);
        ++n;
    }
    std::fclose(f);

    g_api->Log("[homes] loaded %d home(s) for %d player(s).", n, int(g_data.size()));
    if (bad > 0)
        g_api->Log("[homes] %d line(s) could not be read and were SKIPPED, not "
                   "dropped — they are still in the file.", bad);
}

// ── finding a player again, later ───────────────────────────────────────────
//
// The countdown fires seconds after the command, and by then the controller
// pointer taken back then may belong to somebody who logged out. So the player
// is looked up again by id, every time. A stale pointer here would be a crash
// on the game thread, not a wrong message.
static void* FindController(const std::string& id)
{
    static void* buf[256];
    const int n = g_api->FindObjects("ConanPlayerController", buf, 256, /*subclasses=*/1);
    for (int i = 0; i < n; ++i)
    {
        if (!buf[i]) continue;
        char name[128] = {0};
        if (g_api->NomeDoObjeto(buf[i], name, sizeof(name)) &&
            std::strncmp(name, "Default__", 9) == 0) continue;   // the CDO is nobody
        if (PlayerId(buf[i]) == id) return buf[i];
    }
    return nullptr;
}

static float HealthOf(void* character)
{
    if (!character) return 0.0f;
    const float v = ConanApi::Call<float>(character, "GetHealth");
    return g_api->UltimaChamadaExecutou() ? v : 0.0f;
}

// ── in combat? ──────────────────────────────────────────────────────────────
//
// Two sources, and the cheaper one first. `HasCombatTarget` is the game's own
// flag and costs a field read; the timestamp comes from the damage hook and is
// what catches the player who just swung at somebody and has not been hit back.
static double CombatLeft(void* character)
{
    if (!g_blockInCombat || !character) return 0.0;

    const int32_t off = g_api->OffsetDoMembro(character, "HasCombatTarget");
    if (off >= 0 && g_api->LerBit(character, uint32_t(off), 1) > 0)
        return double(g_combatSeconds);

    std::map<void*, double>::const_iterator it = g_combat.find(character);
    if (it == g_combat.end()) return 0.0;
    const double left = (it->second + double(g_combatSeconds)) - Now();
    return left > 0.0 ? left : 0.0;
}

// ── the damage hook ─────────────────────────────────────────────────────────
//
// `HandleTakeDamage` fires on the combat component, not on the character, so
// the owner is resolved before anything is recorded. Both sides are marked: the
// one who was hit AND the one who hit — a player who just attacked is in combat
// whether or not anything touched them.
extern "C" ConanAcao OnTakeDamage(ConanChamada* c)
{
    if (!c || !c->Obj || !g_api) return CONAN_CONTINUAR;

    // Damage, DamageType, InstigatedBy, DamageCauser — the third is the
    // controller that caused it.
    double dano = 0;
    if (g_api->LerParm(c, 0, &dano, sizeof(dano)) <= 0 || dano <= 0.0)
        return CONAN_CONTINUAR;

    const double now = Now();

    void* goal = ConanApi::Call<void*>(c->Obj, "GetOwner");
    if (g_api->UltimaChamadaExecutou() && goal && g_api->Legivel(goal, 8))
        g_combat[goal] = now;

    void* quemCausou = nullptr;
    if (g_api->LerParm(c, 2, &quemCausou, sizeof(quemCausou)) > 0 &&
        quemCausou && g_api->Legivel(quemCausou, 8))
    {
        // InstigatedBy is a controller; the character is what the rest of this
        // plugin keys on.
        if (void* p = CharacterOf(quemCausou)) g_combat[p] = now;
        else                                  g_combat[quemCausou] = now;
    }

    // The map must not grow forever with characters that died or logged out.
    if (g_combat.size() > 512)
    {
        const double limite = now - double(g_combatSeconds) - 5.0;
        for (std::map<void*, double>::iterator it = g_combat.begin(); it != g_combat.end(); )
            if (it->second < limite) g_combat.erase(it++); else ++it;
    }

    return CONAN_CONTINUAR;   // never cancel damage: this hook only watches
}

// ── the commands ────────────────────────────────────────────────────────────

static bool ValidName(const std::string& n)
{
    if (n.empty() || n.size() > 20) return false;
    for (size_t i = 0; i < n.size(); ++i)
    {
        const char ch = n[i];
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                        (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
        if (!ok) return false;
    }
    return true;
}

static Home* FindHome(Player& j, const std::string& name)
{
    for (size_t i = 0; i < j.homes.size(); ++i)
    {
#ifdef _WIN32
        if (_stricmp(j.homes[i].name.c_str(), name.c_str()) == 0) return &j.homes[i];
#else
        if (strcasecmp(j.homes[i].name.c_str(), name.c_str()) == 0) return &j.homes[i];
#endif
    }
    return nullptr;
}

static void CmdAdd(void* controller, const std::string& id, const std::string& name)
{
    if (!ValidName(name))
    {
        Reply(controller, Msg("invalid_name",
              "Invalid name. Use letters, numbers, _ and - (1 to 20 characters)."));
        return;
    }

    Player& j = g_data[id];
    if (int(j.homes.size()) >= g_maxHomes)
    {
        Reply(controller, Fill(Msg("limit_reached",
              "You already have {max} homes. Remove one before creating another."),
              "{max}", Num(g_maxHomes)));
        return;
    }
    if (FindHome(j, name))
    {
        Reply(controller, Fill(Msg("already_exists",
              "You already have a home called '{name}'."), "{name}", name));
        return;
    }

    void* character = CharacterOf(controller);
    FVec where;
    if (!character || !ReadPosition(character, where))
    {
        // Saving (0,0,0) because the game did not answer would hand the player
        // a home that teleports them into the void. Refusing is the honest move.
        Reply(controller, Msg("no_position",
              "I could not read your position. Try again in a few seconds."));
        g_api->Log("[homes] K2_GetActorLocation did not answer for %s — home NOT saved.",
                   id.c_str());
        return;
    }

    Home h;
    h.name = name;
    h.where = where;
    h.yaw  = 0;
    j.homes.push_back(h);
    if (j.displayName.empty())
    {
        char nm[128] = {0};
        if (g_api->NomeDoObjeto(character, nm, sizeof(nm)) && nm[0]) j.displayName = nm;
    }
    Save();

    Reply(controller, Fill(Msg("created", "Home '{name}' created. ({used}/{max})"),
          "{name}", name, "{used}", Num(long(j.homes.size())), "{max}", Num(g_maxHomes)));
    g_api->Log("[homes] %s saved \"%s\" at %.0f %.0f %.0f",
               id.c_str(), name.c_str(), where.x, where.y, where.z);
}

static void CmdRemove(void* controller, const std::string& id, const std::string& name)
{
    std::map<std::string, Player>::iterator it = g_data.find(id);
    if (it == g_data.end() || !FindHome(it->second, name))
    {
        Reply(controller, Fill(Msg("not_found",
              "You have no home called '{name}'."), "{name}", name));
        return;
    }
    std::vector<Home>& homes = it->second.homes;
    for (size_t i = 0; i < homes.size(); ++i)
    {
#ifdef _WIN32
        const bool igual = _stricmp(homes[i].name.c_str(), name.c_str()) == 0;
#else
        const bool igual = strcasecmp(homes[i].name.c_str(), name.c_str()) == 0;
#endif
        if (igual) { homes.erase(homes.begin() + long(i)); break; }
    }
    Save();
    Reply(controller, Fill(Msg("removed", "Home '{name}' removed."), "{name}", name));
}

static void CmdList(void* controller, const std::string& id)
{
    std::map<std::string, Player>::const_iterator it = g_data.find(id);
    if (it == g_data.end() || it->second.homes.empty())
    {
        Reply(controller, Fill(Msg("list_empty",
              "You have no homes. Create one with {cmd} {add} <name>."),
              "{cmd}", g_cmdRoot, "{add}", g_subAdd));
        return;
    }
    std::string line = Fill(Msg("list_title", "Your homes ({used}/{max}):"),
                             "{used}", Num(long(it->second.homes.size())),
                             "{max}", Num(g_maxHomes));
    for (size_t i = 0; i < it->second.homes.size(); ++i)
    {
        line += (i == 0 ? "  " : ", ");
        line += it->second.homes[i].name;
    }
    Reply(controller, line);
}

static void CmdGo(void* controller, const std::string& id, const std::string& name)
{
    std::map<std::string, Player>::iterator it = g_data.find(id);
    Home* casa = (it == g_data.end()) ? nullptr : FindHome(it->second, name);
    if (!casa)
    {
        Reply(controller, Fill(Msg("not_found",
              "You have no home called '{name}'."), "{name}", name));
        return;
    }

    if (g_pending.find(id) != g_pending.end())
    {
        Reply(controller, Msg("already_pending", "You already have a trip under way."));
        return;
    }

    const double now = Now();
    std::map<std::string, double>::const_iterator cd = g_lastTrip.find(id);
    if (cd != g_lastTrip.end())
    {
        const double left = (cd->second + double(g_cooldown)) - now;
        if (left > 0.0)
        {
            Reply(controller, Fill(Msg("cooldown",
                  "Wait {sec}s before travelling again."),
                  "{sec}", Num(long(left) + 1)));
            return;
        }
    }

    void* character = CharacterOf(controller);
    const double combat = CombatLeft(character);
    if (combat > 0.0)
    {
        Reply(controller, Fill(Msg("in_combat",
              "You are in COMBAT. Wait {sec}s out of combat."),
              "{sec}", Num(long(combat) + 1)));
        return;
    }

    Pending p;
    p.name        = casa->name;
    p.target     = casa->where;
    p.firesAt     = now + double(g_waitSeconds);
    p.healthAtStart = HealthOf(character);
    g_pending[id] = p;

    Reply(controller, Fill(Msg("starting", "Travelling to '{name}' in {sec}s. Hold still."),
          "{name}", casa->name, "{sec}", Num(g_waitSeconds)));
}

// ── the countdown ───────────────────────────────────────────────────────────
//
// Runs on the GAME THREAD — the API guarantees it, and touching a game object
// from anywhere else corrupts the world slowly and without an error.
//
// The order matters at the end: the player moves FIRST, then the mount, then
// the followers. Moving a follower to where the player is about to be, rather
// than where they are, is what keeps the group together.
static void Tick(void*)
{
    if (g_pending.empty()) return;
    const double now = Now();

    std::vector<std::string> gone;

    for (std::map<std::string, Pending>::iterator it = g_pending.begin();
         it != g_pending.end(); ++it)
    {
        const std::string& id = it->first;
        Pending& p = it->second;

        void* controller = FindController(id);
        if (!controller) { gone.push_back(id); continue; }   // logged out

        void* character = CharacterOf(controller);
        if (!character)
        {
            Reply(controller, Msg("cancelled_death", "Trip cancelled: you died."));
            gone.push_back(id);
            continue;
        }

        if (g_cancelOnDamage)
        {
            const float vida = HealthOf(character);
            // Only a DROP cancels. Healing during the wait is not an attack, and
            // a plugin that cancelled on any change would be unusable next to a
            // campfire.
            if (vida > 0.0f && vida < p.healthAtStart - 0.5f)
            {
                Reply(controller, Msg("cancelled_damage", "Trip cancelled: you took damage."));
                gone.push_back(id);
                continue;
            }
        }

        if (now < p.firesAt) continue;

        const bool went = TeleportPlayer(controller, p.target, 0.0);
        if (!went)
        {
            Reply(controller, Msg("failed", "I could not take you there. Tell an admin."));
            g_api->Log("[homes] TeleportPlayerServer did not answer for %s", id.c_str());
            gone.push_back(id);
            continue;
        }

        const int mount   = BringMount(character, p.target);
        const int followers = BringFollowers(controller, character, p.target);

        g_lastTrip[id] = now;
        gone.push_back(id);

        Reply(controller, Fill(Msg("arrived", "Welcome to '{name}'."), "{name}", p.name));
        g_api->Log("[homes] %s -> \"%s\" (%.0f %.0f %.0f) · mount %s · %d follower(s)",
                   id.c_str(), p.name.c_str(),
                   p.target.x, p.target.y, p.target.z,
                   mount ? "yes" : "no", followers);
    }

    for (size_t i = 0; i < gone.size(); ++i) g_pending.erase(gone[i]);
}

// ── may this player use it? ─────────────────────────────────────────────────
static bool MayRun(const std::string& id, std::string& why)
{
    if (g_permission.empty()) return true;
    const ConanPermApi* perm = ConanPermObter();
    if (!perm || !perm->tem)
    {
        why = "config.json requires \"" + g_permission +
                 "\" but ConanPermission.dll is not installed";
        return false;
    }
    if (ConanPermTem(id.c_str(), g_permission.c_str(), /*if_absent=*/0) != 1)
    {
        why = "the player does not have the node \"" + g_permission + "\"";
        return false;
    }
    return true;
}

// ── the chat hook ───────────────────────────────────────────────────────────
//
// THE PREFIX IS `!`, NOT `/`. Conan's client swallows `/command` locally and
// never sends it, so no plugin in any API gets to see it. Measured on a live
// server.
static std::vector<std::string> Words(const char* t)
{
    std::vector<std::string> r;
    std::string atual;
    for (const char* p = t; *p; ++p)
    {
        if (*p == ' ' || *p == '\t') { if (!atual.empty()) { r.push_back(atual); atual.clear(); } }
        else atual += *p;
    }
    if (!atual.empty()) r.push_back(atual);
    return r;
}

extern "C" ConanAcao OnChat(ConanChamada* c)
{
    if (!c || !c->Parms || c->ParmsSize < 0x80) return CONAN_CONTINUAR;

    char texto[256];
    texto[0] = 0;
    if (g_api->LerTextoDoJogo(c->Parms, CHAT_TEXT, texto, sizeof(texto)) <= 0)
        return CONAN_CONTINUAR;

    const std::vector<std::string> w = Words(texto);
    if (w.empty()) return CONAN_CONTINUAR;
    if (w[0] != g_cmdRoot && w[0] != g_cmdList) return CONAN_CONTINUAR;

    void* controller = c->Obj;
    const std::string id = PlayerId(controller);
    if (id.empty())
    {
        // Without an id there is nothing to key a home on, and guessing would
        // hand one player another player's houses.
        Reply(controller, Msg("no_id",
              "I could not identify your account. Tell an admin."));
        g_api->Log("[homes] no stable id for this controller — is Permission installed?");
        return CONAN_CANCELAR;
    }

    std::string why;
    if (!MayRun(id, why))
    {
        g_api->Log("[homes] refused for %s: %s", id.c_str(), why.c_str());
        Reply(controller, Msg("no_permission", "You are not allowed to use homes."));
        return CONAN_CANCELAR;
    }

    if (w[0] == g_cmdList) { CmdList(controller, id); return CONAN_CANCELAR; }

    if (w.size() == 1)
    {
        Reply(controller, Fill(Msg("usage",
              "Use: {cmd} {add} <name> · {cmd} <name> · {cmd} {del} <name>"),
              "{cmd}", g_cmdRoot, "{add}", g_subAdd, "{del}", g_subDel));
        return CONAN_CANCELAR;
    }

    if (w[1] == g_subAdd)
    {
        if (w.size() < 3) Reply(controller, Fill(Msg("usage_add", "Use: {cmd} {add} <name>"),
                                "{cmd}", g_cmdRoot, "{add}", g_subAdd));
        else              CmdAdd(controller, id, w[2]);
        return CONAN_CANCELAR;
    }
    if (w[1] == g_subDel)
    {
        if (w.size() < 3) Reply(controller, Fill(Msg("usage_del", "Use: {cmd} {del} <name>"),
                                "{cmd}", g_cmdRoot, "{del}", g_subDel));
        else              CmdRemove(controller, id, w[2]);
        return CONAN_CANCELAR;
    }

    CmdGo(controller, id, w[1]);
    return CONAN_CANCELAR;
}

// ── config.json ─────────────────────────────────────────────────────────────
//
// The parser is deliberately small: flat keys, strings, numbers and booleans,
// no nesting. A COLON MUST FOLLOW THE KEY — the shipped config opens with a
// comment block that names every setting, and "first place this word appears"
// would load a sentence out of the documentation as if it were a setting.
static std::string g_configText;

static bool FindKey(const char* chave, size_t& afterColon)
{
    const std::string goal = std::string("\"") + chave + "\"";
    for (size_t p = g_configText.find(goal); p != std::string::npos;
         p = g_configText.find(goal, p + 1))
    {
        size_t c = p + goal.size();
        while (c < g_configText.size() && (g_configText[c] == ' ' || g_configText[c] == '\t')) ++c;
        if (c >= g_configText.size() || g_configText[c] != ':') continue;   // a mention
        afterColon = c + 1;
        return true;
    }
    return false;
}

static void ReadText(const char* chave, std::string& target)
{
    size_t c = 0;
    if (!FindKey(chave, c)) return;
    const size_t a = g_configText.find('"', c);
    if (a == std::string::npos) return;
    std::string v;
    size_t b = a + 1;
    while (b < g_configText.size() && g_configText[b] != '"')
    {
        if (g_configText[b] == '\\' && b + 1 < g_configText.size()) ++b;
        v += g_configText[b++];
    }
    if (b < g_configText.size()) target = v;
}

static void ReadInt(const char* chave, int& target)
{
    size_t c = 0;
    if (!FindKey(chave, c)) return;
    target = int(std::strtol(g_configText.c_str() + c, nullptr, 10));
}

static void ReadBool(const char* chave, bool& target)
{
    size_t c = 0;
    if (!FindKey(chave, c)) return;
    while (c < g_configText.size() && (g_configText[c] == ' ' || g_configText[c] == '\t')) ++c;
    target = (g_configText.compare(c, 4, "true") == 0);
}

static void ReadMessage(const char* chave)
{
    std::string v;
    ReadText(chave, v);
    if (!v.empty()) g_msg[chave] = v;
}

static void ReadConfig()
{
    if (!g_api || !g_api->CaminhoConfig) return;
    const char* path = g_api->CaminhoConfig("ConanHomes");
    if (!path) return;

    FILE* f = nullptr;
    CONAN_FOPEN(f, path, "rb");
    if (!f) { g_api->Log("[homes] no config.json — using the defaults."); return; }
    g_configText.clear();
    {
        char buf[4096]; size_t r;
        while ((r = std::fread(buf, 1, sizeof(buf), f)) > 0) g_configText.append(buf, r);
    }
    std::fclose(f);

    ReadText("command",       g_cmdRoot);
    ReadText("command_list", g_cmdList);
    ReadText("sub_add",       g_subAdd);
    ReadText("sub_remove",   g_subDel);
    ReadText("permission",     g_permission);

    ReadInt("max_homes",       g_maxHomes);
    ReadInt("wait_sec",      g_waitSeconds);
    ReadInt("cooldown_sec",     g_cooldown);
    ReadInt("combat_sec",     g_combatSeconds);
    ReadBool("bring_mount", g_takeMount);
    ReadBool("bring_followers", g_takeFollowers);
    ReadBool("cancel_on_damage", g_cancelOnDamage);
    ReadBool("block_in_combat",    g_blockInCombat);

    ReadInt("cost_add",    g_costAdd);
    ReadInt("cost_travel", g_costGo);
    ReadInt("cost_remove", g_costDel);

    static const char* const MENSAGENS[] = {
        "invalid_name", "limit_reached", "already_exists", "no_position", "created",
        "not_found", "removed", "list_empty", "list_title", "already_pending",
        "cooldown", "in_combat", "starting", "arrived", "cancelled_damage",
        "cancelled_death", "failed", "no_permission", "no_id", "usage",
        "usage_add", "usage_del", nullptr
    };
    for (int i = 0; MENSAGENS[i]; ++i) ReadMessage(MENSAGENS[i]);

    g_api->Log("[homes] config: %s / %s · max %d · wait %ds · cooldown %ds · "
               "mount %s · followers %s · combat %s",
               g_cmdRoot.c_str(), g_cmdList.c_str(), g_maxHomes,
               g_waitSeconds, g_cooldown,
               g_takeMount ? "yes" : "no",
               g_takeFollowers ? "yes" : "no",
               g_blockInCombat ? "blocked" : "free");

    if (g_costAdd || g_costGo || g_costDel)
        g_api->Log("[homes] WARNING: costs are configured (%d/%d/%d) but NOTHING is charged: "
                   "there is no inter-plugin points API yet. The fields stay for "
                   "the day there is one.",
                   g_costAdd, g_costGo, g_costDel);
}

extern "C" __declspec(dllexport)
void ConanPluginCarregar(const ConanApiTabela* api)
{
    if (!api || api->tamanho < sizeof(ConanApiTabela)) return;
    g_api = api;
    ConanApi::UsarTabela(api);

    g_api->Log("");
    g_api->Log("=== ConanHomes ===");

    if (!g_api->Pronta())
    {
        g_api->Log("[homes] ABORTED: reflection unavailable. Did the game update?");
        return;
    }

    ReadConfig();
    Load();

    const uint32_t hChat = g_api->HookProcessEvent("ServerSendChatMessage",
                                                   OnChat, nullptr, 100);
    if (!hChat)
    {
        g_api->Log("[homes] could not hook the chat; the commands will not work.");
        return;
    }

    // Optional: without it, "block in combat" falls back to the game's own
    // HasCombatTarget flag and loses the "I just hit someone" half. The plugin
    // still works, and the log says which half is missing rather than pretending.
    if (g_blockInCombat)
    {
        const uint32_t hDano = g_api->HookProcessEvent("HandleTakeDamage",
                                                       OnTakeDamage, nullptr, 100);
        if (!hDano)
            g_api->Log("[homes] no HandleTakeDamage hook: \"in combat\" falls back to the "
                       "game's own HasCombatTarget. Somebody who ATTACKED and was "
                       "not hit back will not be blocked.");
    }

    // 1 second: the countdown is measured in whole seconds, so anything finer
    // would be work nobody can see.
    g_api->AgendarNaThreadDoJogo(Tick, 1, nullptr, /*repetir=*/1);

    g_api->Log("[homes] ready: %s %s <name> · %s <name> · %s %s <name> · %s",
               g_cmdRoot.c_str(), g_subAdd.c_str(),
               g_cmdRoot.c_str(),
               g_cmdRoot.c_str(), g_subDel.c_str(),
               g_cmdList.c_str());
}

extern "C" __declspec(dllexport)
void ConanPluginDescarregar(void)
{
    // Honest about today: ConanLoader does not call this — the server process
    // dies whole. Saving here would be a comfort, not a guarantee, which is why
    // every change is written the moment it happens instead.
    if (g_api) g_api->Log("[homes] unloading.");
    g_api = nullptr;
}
