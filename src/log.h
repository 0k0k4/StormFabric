#pragma once
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace swml {

enum class LogLevel { Debug = 0, Info = 1, Warn = 2, Error = 3 };

struct ModLoaderConfig {
    bool openTerminalOnStartup = true;  // Ouvre une console Windows de logs dès le lancement du jeu
    bool showNotifications = true;      // Affiche les notifications en haut à gauche dans l'UI
    bool notifInfo = true;              // Affiche les notifications INFO
    bool notifWarn = true;              // Affiche les notifications WARN
    bool notifError = true;             // Affiche les notifications ERROR
    bool verboseDebug = true;           // Journalise les détails techniques (RVA, DLLs, hooks...)
    bool hotReloadMods = true;          // Charge/décharge les DLLs à chaud au clic dans l'UI
    bool autoOpenModsMenu = false;      // Ouvre la vue MODS automatiquement au démarrage
};

struct LogEntry {
    uint64_t id = 0;
    std::string timeStr;   // ex: "22:31:07.229"
    LogLevel level = LogLevel::Info;
    std::string message;
};

struct NotificationItem {
    uint64_t id = 0;
    LogLevel level = LogLevel::Info;
    std::string title;
    std::string detail;
    uint64_t expireTickMs = 0;
};

// Initialise le système de journalisation, charge ModLoaderConfig depuis mods\_modloader.ini,
// et ouvre immédiatement le terminal de logs si `openTerminalOnStartup` est activé.
void LogInit();

// Accès et sauvegarde de la configuration globale du ModLoader
ModLoaderConfig& GetConfig();
void SaveConfig();

// Contrôle du terminal de logs externe (console Windows)
void OpenLogTerminal();
void CloseLogTerminal();
bool IsLogTerminalOpen();
void SetLogTerminalOpen(bool open);

// Journalisation (fichier StormworksModLoader.log + terminal couleur + buffer mémoire UI)
void Log(const char* fmt, ...);
void LogLevelMsg(LogLevel level, const char* fmt, ...);
void LogDebug(const char* fmt, ...);
void LogInfo(const char* fmt, ...);
void LogWarn(const char* fmt, ...);
void LogError(const char* fmt, ...);

// Discovery-only progress; never opens a terminal. total == 0 is indeterminate.
// completed >= total (with total > 0) finishes this phase, not compatibility checks.
// Keep detail stable during byte scans; change it for symbol/status checkpoints.
void LogDiscoveryProgress(const char* phase, size_t completed, size_t total,
                          const char* detail, bool failed = false);

// Ajoute une notification en haut à gauche de l'écran (et l'écrit aussi dans les logs).
// Si durationSec <= 0, utilise la durée par défaut du niveau (Info=4.5s, Warn=6.5s, Error=8.5s).
void Notify(LogLevel level, const std::string& title, const std::string& detail,
            float durationSec = 0.0f);

// Récupère les notifications actives non expirées (max `maxCount`), et retourne un numéro de
// révision qui change uniquement quand la liste visible évolue.
uint64_t GetActiveNotifications(std::vector<NotificationItem>& out, size_t maxCount = 4);
void ClearNotifications();

// Historique des logs pour la visionneuse intégrée à l'UI
std::vector<LogEntry> GetRecentLogs(size_t maxCount = 60);
void ClearLogs();
void OpenLogFile();

}  // namespace swml
