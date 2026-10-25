// Text the recomp adds in French, German, Italian and Spanish: Double Agent recomp's wording where the English is the
// same, in sentence case with German's nouns capitalized; the panel's rows and the game's menu show it in capitals.
#include <algorithm>
#include <cctype>
#include <iterator>
#include <map>
#include <mutex>

#include "common/options.h"
#include "common/translations.h"

namespace {

// in game::kLanguages order after int
struct Entry {
    std::string_view english;
    std::u16string_view text[4];
};
constexpr Entry kEntries[] = {
    // options menu pages and the RECOMP SETTINGS sections
    {"RECOMP SETTINGS", {u"Paramètres Recomp", u"Recomp-Einstellungen", u"Impostazioni Recomp", u"Configuración de Recomp"}},
    {"MOUSE AND KEYBOARD", {u"Souris et clavier", u"Maus und Tastatur", u"Mouse e tastiera", u"Ratón y teclado"}},
    {"CONTROLLER", {u"Manette", u"Controller", u"Controller", u"Mando"}},
    {"GENERAL", {u"Général", u"Allgemein", u"Generale", u"General"}},
    {"DISPLAY", {u"Affichage", u"Anzeige", u"Video", u"Pantalla"}},
    {"GRAPHICS", {u"Graphismes", u"Grafik", u"Grafica", u"Gráficos"}},
    {"RESTORE DEFAULTS", {u"Rétablir les valeurs par défaut", u"Standard wiederherstellen", u"Ripristina predefiniti", u"Restaurar predeterminados"}},
    {"PRESS A KEY", {u"Appuyez sur une touche", u"Taste drücken", u"Premi un tasto", u"Pulsa una tecla"}},
    {"UNBOUND", {u"Non assigné", u"Nicht belegt", u"Non assegnato", u"Sin asignar"}},

    // RECOMP SETTINGS rows
    {"MONITOR", {u"Écran", u"Monitor", u"Monitor", u"Monitor"}},
    {"DISPLAY MODE", {u"Mode d'affichage", u"Anzeigemodus", u"Modalità schermo", u"Modo de pantalla"}},
    {"RESOLUTION", {u"Résolution", u"Auflösung", u"Risoluzione", u"Resolución"}},
    {"RENDER RESOLUTION", {u"Résolution de rendu", u"Render-Auflösung", u"Risoluzione di rendering", u"Resolución de renderizado"}},
    {"FIELD OF VIEW", {u"Champ de vision", u"Sichtfeld", u"Campo visivo", u"Campo de visión"}},
    {"VIEWMODEL FIELD OF VIEW", {u"Champ de vision de l'arme", u"Waffen-Sichtfeld", u"Campo visivo dell'arma", u"Campo de visión del arma"}},
    {"VERTICAL SYNC", {u"Synchro verticale", u"V-Sync", u"Sincronia verticale", u"Sincronismo vertical"}},
    {"FMV SCALE", {u"Échelle des cinématiques", u"Videoskalierung", u"Scala dei filmati", u"Escala de vídeos"}},
    {"FILL", {u"Remplir", u"Füllen", u"Riempi", u"Rellenar"}},
    {"COLOUR BLIND MODE", {u"Mode daltonien", u"Farbenblindmodus", u"Modalità daltonici", u"Modo daltónico"}},
    {"PROTANOPIA", {u"Protanopie", u"Protanopie", u"Protanopia", u"Protanopía"}},
    {"DEUTERANOPIA", {u"Deutéranopie", u"Deuteranopie", u"Deuteranopia", u"Deuteranopía"}},
    {"TRITANOPIA", {u"Tritanopie", u"Tritanopie", u"Tritanopia", u"Tritanopía"}},
    {"HDR OUTPUT", {u"Sortie HDR", u"HDR-Ausgabe", u"Uscita HDR", u"Salida HDR"}},
    {"MAX FRAME RATE", {u"Images par seconde max.", u"Max. Bildrate", u"Frame rate massimo", u"Fotogramas por segundo máx."}},
    {"FPS COUNTER", {u"Compteur d'images par seconde", u"FPS-Anzeige", u"Contatore FPS", u"Contador de FPS"}},
    {"ANISOTROPIC FILTERING", {u"Filtrage anisotropique", u"Anisotropische Filterung", u"Filtro anisotropico", u"Filtro anisotrópico"}},
    {"DRAW DISTANCE", {u"Distance d'affichage", u"Sichtweite", u"Distanza visiva", u"Distancia de dibujado"}},
    {"AMBIENT OCCLUSION", {u"Occlusion ambiante", u"Ambient Occlusion", u"Occlusione ambientale", u"Oclusión ambiental"}},
    {"RELOAD BLUR", {u"Flou au rechargement", u"Nachlade-Unschärfe", u"Sfocatura in ricarica", u"Desenfoque al recargar"}},
    {"SKIP INTROS", {u"Passer les intros", u"Intros überspringen", u"Salta introduzione", u"Saltar introducción"}},
    {"MENU ANIMATIONS", {u"Animations des menus", u"Menüanimationen", u"Animazioni dei menu", u"Animaciones de los menús"}},
    {"PAUSE ON FOCUS LOSS", {u"Pause en arrière-plan", u"Pause im Hintergrund", u"Pausa in background", u"Pausa en segundo plano"}},
    {"LANGUAGE", {u"Langue", u"Sprache", u"Lingua", u"Idioma"}},
    {"AUDIO OUTPUT", {u"Sortie audio", u"Audioausgabe", u"Uscita audio", u"Salida de audio"}},

    // their values
    {"FULLSCREEN", {u"Plein écran", u"Vollbild", u"Schermo intero", u"Pantalla completa"}},
    {"WINDOWED", {u"Fenêtré", u"Fenster", u"Finestra", u"Ventana"}},
    {"BORDERLESS", {u"Fenêtré sans bordure", u"Randloses Fenster", u"Finestra senza bordi", u"Ventana sin bordes"}},
    {"ON", {u"Oui", u"Ein", u"Sì", u"Sí"}},
    {"OFF", {u"Non", u"Aus", u"No", u"No"}},
    {"UNLIMITED", {u"Illimité", u"Unbegrenzt", u"Illimitato", u"Ilimitado"}},
    {"REFRESH RATE", {u"Fréquence de rafraîchissement", u"Bildwiederholfrequenz", u"Frequenza di aggiornamento", u"Frecuencia de actualización"}},
    {"AUTOMATIC", {u"Automatique", u"Automatisch", u"Automatico", u"Automático"}},
    {"GAME DEFAULT", {u"Par défaut du jeu", u"Spielstandard", u"Predefinito del gioco", u"Predeterminado del juego"}},
    {"NOT SUPPORTED", {u"Non pris en charge", u"Nicht unterstützt", u"Non supportato", u"No compatible"}},
    {"MONO", {u"Mono", u"Mono", u"Mono", u"Mono"}},
    {"2.0 STEREO", {u"2.0 Stéréo", u"2.0 Stereo", u"2.0 Stereo", u"2.0 Estéreo"}},
    {"5.1 SURROUND", {u"5.1 Surround", u"5.1 Surround", u"5.1 Surround", u"5.1 Envolvente"}},
    {"(DEFAULT)", {u"(par défaut)", u"(Standard)", u"(predefinito)", u"(por defecto)"}},
    {"(RESTART)", {u"(redémarrage)", u"(Neustart)", u"(riavvio)", u"(reinicio)"}},
    {"PRIMARY", {u"Principal", u"Primär", u"Principale", u"Principal"}},
    {"(PRIMARY)", {u"(principal)", u"(primär)", u"(principale)", u"(principal)"}},
    {"ENGLISH", {u"Anglais", u"Englisch", u"Inglese", u"Inglés"}},
    {"FRENCH", {u"Français", u"Französisch", u"Francese", u"Francés"}},
    {"GERMAN", {u"Allemand", u"Deutsch", u"Tedesco", u"Alemán"}},
    {"ITALIAN", {u"Italien", u"Italienisch", u"Italiano", u"Italiano"}},
    {"SPANISH", {u"Espagnol", u"Spanisch", u"Spagnolo", u"Español"}},

    // MOUSE AND KEYBOARD rows
    {"MOUSE SENSITIVITY", {u"Sensibilité de la souris", u"Mausempfindlichkeit", u"Sensibilità del mouse", u"Sensibilidad del ratón"}},
    {"INVERT LOOK", {u"Inverser la visée", u"Blick umkehren", u"Inverti visuale", u"Invertir vista"}},
    {"HOLD TO AIM", {u"Maintenir pour viser", u"Zum Zielen halten", u"Tieni premuto per mirare", u"Mantener para apuntar"}},
    {"WALK SPEED", {u"Vitesse de marche", u"Gehgeschwindigkeit", u"Velocità di camminata", u"Velocidad al caminar"}},
    {"MOVE FORWARD", {u"Avancer", u"Vorwärts bewegen", u"Avanti", u"Avanzar"}},
    {"MOVE BACK", {u"Reculer", u"Rückwärts bewegen", u"Indietro", u"Retroceder"}},
    {"STRAFE LEFT", {u"Pas latéral à gauche", u"Seitwärts nach links", u"Spostamento laterale a sinistra", u"Desplazamiento lateral a la izquierda"}},
    {"STRAFE RIGHT", {u"Pas latéral à droite", u"Seitwärts nach rechts", u"Spostamento laterale a destra", u"Desplazamiento lateral a la derecha"}},
    {"WALK", {u"Marcher", u"Gehen", u"Cammina", u"Caminar"}},
    {"CROUCH", {u"S'accroupir", u"Ducken", u"Chinati", u"Agacharse"}},
    {"FIRE", {u"Tirer", u"Feuern", u"Fuoco", u"Disparar"}},
    {"AIM", {u"Viser", u"Zielen", u"Mira", u"Apuntar"}},
    {"MELEE", {u"Corps à corps", u"Nahkampf", u"Corpo a corpo", u"Cuerpo a cuerpo"}},
    {"RELOAD", {u"Recharger", u"Nachladen", u"Ricarica", u"Recargar"}},
    {"INTERACT", {u"Action", u"Aktion", u"Azione", u"Acción"}},
    {"THROW GRENADE", {u"Lancer une grenade", u"Granate werfen", u"Lancia granata", u"Lanzar granada"}},
    {"CHANGE WEAPON UP", {u"Arme suivante", u"Nächste Waffe", u"Arma successiva", u"Arma siguiente"}},
    {"CHANGE WEAPON DOWN", {u"Arme précédente", u"Vorige Waffe", u"Arma precedente", u"Arma anterior"}},
    {"TOGGLE SUPPRESSOR", {u"Mettre / retirer le silencieux", u"Schalldämpfer an / ab", u"Monta / smonta silenziatore", u"Poner / quitar silenciador"}},
    {"FIRING MODE / SCOPE ZOOM", {u"Mode de tir / zoom de la lunette", u"Feuermodus / Zielfernrohr-Zoom", u"Modalità di fuoco / Zoom mirino", u"Modo de disparo / Zoom de la mira"}},
    {"HEALTH PACK", {u"Trousse de soins", u"Medipack", u"Kit medico", u"Botiquín"}},
    {"NEXT MUSIC TRACK", {u"Morceau suivant", u"Nächster Musiktitel", u"Brano successivo", u"Siguiente pista de música"}},
    {"OBJECTIVES", {u"Objectifs", u"Ziele", u"Obiettivi", u"Objetivos"}},
    {"PAUSE MENU", {u"Menu pause", u"Pausenmenü", u"Menu di pausa", u"Menú de pausa"}},
    {"TOGGLE HUD", {u"Afficher / masquer l'interface", u"HUD ein / aus", u"Mostra / nascondi HUD", u"Mostrar / ocultar HUD"}},
    {"SETTINGS PANEL", {u"Panneau des paramètres", u"Einstellungsmenü", u"Pannello impostazioni", u"Panel de configuración"}},
    {"LOOK UP", {u"Regarder en haut", u"Nach oben sehen", u"Guarda in alto", u"Mirar arriba"}},
    {"LOOK DOWN", {u"Regarder en bas", u"Nach unten sehen", u"Guarda in basso", u"Mirar abajo"}},
    {"LOOK LEFT", {u"Regarder à gauche", u"Nach links sehen", u"Guarda a sinistra", u"Mirar a la izquierda"}},
    {"LOOK RIGHT", {u"Regarder à droite", u"Nach rechts sehen", u"Guarda a destra", u"Mirar a la derecha"}},

    // CONTROLLER rows
    {"SENSITIVITY", {u"Sensibilité", u"Empfindlichkeit", u"Sensibilità", u"Sensibilidad"}},
    {"RUMBLE INTENSITY", {u"Intensité des vibrations", u"Vibrationsstärke", u"Intensità della vibrazione", u"Intensidad de la vibración"}},
    {"AIM ASSIST", {u"Aide à la visée", u"Zielhilfe", u"Mira assistita", u"Asistencia de apuntado"}},
    {"GYRO", {u"Gyroscope", u"Gyroskop", u"Giroscopio", u"Giroscopio"}},
    {"GYRO AIMING", {u"Visée gyroscopique", u"Gyro-Zielen", u"Mira giroscopica", u"Apuntado con giroscopio"}},
    {"GYRO SENSITIVITY", {u"Sensibilité du gyroscope", u"Gyro-Empfindlichkeit", u"Sensibilità del giroscopio", u"Sensibilidad del giroscopio"}},
    {"GYRO AXIS", {u"Axe du gyroscope", u"Gyro-Achse", u"Asse del giroscopio", u"Eje del giroscopio"}},
    {"GYRO BUTTON", {u"Bouton du gyroscope", u"Gyro-Taste", u"Pulsante del giroscopio", u"Botón del giroscopio"}},
    {"GYRO BUTTON MODE", {u"Mode du bouton du gyroscope", u"Gyro-Tastenmodus", u"Modalità pulsante del giroscopio", u"Modo del botón del giroscopio"}},
    {"RECENTER BUTTON", {u"Bouton de recentrage", u"Zentrieren-Taste", u"Pulsante di ricentratura", u"Botón de recentrado"}},
    {"CALIBRATE GYRO", {u"Calibrer le gyroscope", u"Gyro kalibrieren", u"Calibra il giroscopio", u"Calibrar el giroscopio"}},
    {"YAW", {u"Lacet", u"Gieren", u"Imbardata", u"Guiñada"}},
    {"ROLL", {u"Roulis", u"Rollen", u"Rollio", u"Alabeo"}},
    {"WORLD", {u"Monde", u"Welt", u"Mondo", u"Mundo"}},
    {"NONE", {u"Aucun", u"Keine", u"Nessuno", u"Ninguno"}},
    {"TOUCHPAD", {u"Pavé tactile", u"Touchpad", u"Touchpad", u"Panel táctil"}},
    {"MISC", {u"Divers", u"Sonstige", u"Varie", u"Varios"}},
    {"RIGHT PADDLE 1", {u"Palette droite 1", u"Paddle rechts 1", u"Paletta destra 1", u"Palanca derecha 1"}},
    {"LEFT PADDLE 1", {u"Palette gauche 1", u"Paddle links 1", u"Paletta sinistra 1", u"Palanca izquierda 1"}},
    {"RIGHT PADDLE 2", {u"Palette droite 2", u"Paddle rechts 2", u"Paletta destra 2", u"Palanca derecha 2"}},
    {"LEFT PADDLE 2", {u"Palette gauche 2", u"Paddle links 2", u"Paletta sinistra 2", u"Palanca izquierda 2"}},
    {"OFF WHILE HELD", {u"Désactivé si maintenu", u"Aus beim Halten", u"Disattivato se tenuto premuto", u"Desactivado al mantener"}},
    {"ON WHILE HELD", {u"Activé si maintenu", u"Ein beim Halten", u"Attivato se tenuto premuto", u"Activado al mantener"}},
    {"TOGGLE", {u"Basculer", u"Umschalten", u"Alterna", u"Alternar"}},
    {"START", {u"Démarrer", u"Starten", u"Avvia", u"Iniciar"}},
    {"KEEP STILL", {u"Ne bougez pas", u"Nicht bewegen", u"Non muovere", u"No lo muevas"}},

    // a hint's "PRESS (A) :" with the key in place of the button
    {"PRESS", {u"Appuyez sur", u"Drücke", u"Premi", u"Pulsa"}},

    // the F10 panel's own
    {"SETTINGS", {u"Paramètres", u"Einstellungen", u"Impostazioni", u"Configuración"}},
    {"BACK", {u"Retour", u"Zurück", u"Indietro", u"Atrás"}},
    {"DEFAULTS", {u"Par défaut", u"Standard", u"Predefiniti", u"Por defecto"}},
    {"APPLY", {u"Appliquer", u"Übernehmen", u"Applica", u"Aplicar"}},
    {"MOUSE", {u"Souris", u"Maus", u"Mouse", u"Ratón"}},
    {"KEYBOARD", {u"Clavier", u"Tastatur", u"Tastiera", u"Teclado"}},
    {"Press a key or mouse button to bind, Escape cancels", {u"Appuyez sur une touche ou un bouton de souris, Échap pour annuler", u"Taste oder Maustaste drücken, Escape bricht ab", u"Premi un tasto o un pulsante del mouse, Esc per annullare", u"Pulsa una tecla o un botón del ratón, Escape para cancelar"}},
    {"Right click or Delete clears the key", {u"Clic droit ou Suppr pour effacer la touche", u"Rechtsklick oder Entf löscht die Taste", u"Clic destro o Canc per cancellare il tasto", u"Clic derecho o Supr para borrar la tecla"}},
};

bool Same(std::string_view a, std::string_view b) {
    auto upper = [](char c) { return char(std::toupper(uint8_t(c))); };
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [&](char x, char y) {
               return upper(x) == upper(y);
           });
}

bool WordChar(char c) { return std::isalnum(uint8_t(c)) != 0; }

// the language the game booted in, -1 for English
int Language() {
    static const int language = [] {
        if (!game::HasLanguages()) return -1;
        auto at = std::find(std::begin(game::kLanguages), std::end(game::kLanguages), game::BootLanguage());
        return at == std::end(game::kLanguages) ? -1 : int(at - std::begin(game::kLanguages)) - 1;
    }();
    return language;
}

const char16_t* Translation(std::string_view english) {
    for (const Entry& e : kEntries)
        if (Same(e.english, english)) return e.text[Language()].data();
    return nullptr;
}

// the longest phrase with a translation from text[at], at a word start; end set past it
const char16_t* PhraseAt(std::string_view text, size_t at, size_t& end) {
    if (at && WordChar(text[at - 1])) return nullptr;
    for (end = text.size(); end > at; end--) {
        if (end < text.size() && WordChar(text[end])) continue;
        if (const char16_t* found = Translation(text.substr(at, end - at))) return found;
    }
    return nullptr;
}

// latin 1 lower case to upper; 0xF7 is the division sign
char16_t Upper(char16_t c) {
    bool lower = (c >= u'a' && c <= u'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7);
    return lower ? char16_t(c - 0x20) : c;
}

void AppendUtf8(std::string& out, char16_t c) {
    if (c < 0x80) {
        out += char(c);
    } else if (c < 0x800) {
        out += char(0xC0 | c >> 6);
        out += char(0x80 | (c & 0x3F));
    } else {
        out += char(0xE0 | c >> 12);
        out += char(0x80 | ((c >> 6) & 0x3F));
        out += char(0x80 | (c & 0x3F));
    }
}

}  // namespace

std::string app::Ui(std::string_view text, bool caps) {
    if (Language() < 0) return std::string(text);
    // the panel's thread and the game's
    static std::mutex lock;
    static std::map<std::string, std::string> cache;
    std::lock_guard guard(lock);
    std::string key = std::string(text) + (caps ? "\1" : "\2");
    if (auto it = cache.find(key); it != cache.end()) return it->second;
    std::string out;
    size_t i = 0;
    while (i < text.size()) {
        size_t end = 0;
        const char16_t* found = PhraseAt(text, i, end);
        if (!found) {
            out += text[i++];
            continue;
        }
        for (; *found; found++) AppendUtf8(out, caps ? Upper(*found) : *found);
        i = end;
    }
    return cache[key] = out;
}
