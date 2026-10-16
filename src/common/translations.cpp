// Text the recomp adds in French, German, Italian and Spanish: Double Agent recomp's wording where the English is the
// same, in sentence case with German's nouns capitalized; the panel's rows and the game's menu show it in capitals.
#include <algorithm>
#include <chrono>
#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <iomanip>
#include <sstream>
#include <mutex>
#include <string>

#include "app/app.h"
#include "core/log.h"

#include "common/options.h"
#include "common/translations.h"

namespace {

// in game::kLanguages order after int
struct Entry {
    std::string_view english;
    std::u16string_view text[7];
};
constexpr Entry kEntries[] = {
    // options menu pages and the RECOMP SETTINGS sections
    {"RECOMP SETTINGS", {u"Paramètres Recomp", u"Recomp-Einstellungen", u"Impostazioni Recomp", u"Configuración de Recomp"}},
    {"MOUSE AND KEYBOARD", {u"Souris et clavier", u"Maus und Tastatur", u"Mouse e tastiera", u"Ratón y teclado"}},
    {"GENERAL", {u"Général", u"Allgemein", u"Generale", u"General"}},
    {"DISPLAY", {u"Affichage", u"Anzeige", u"Video", u"Pantalla"}},
    {"GRAPHICS", {u"Graphismes", u"Grafik", u"Grafica", u"Gráficos"}},
    {"PAGE", {u"Page", u"Seite", u"Pagina", u"Página"}},
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
    {"HDR OUTPUT", {u"Sortie HDR", u"HDR-Ausgabe", u"Uscita HDR", u"Salida HDR"}},
    {"MAX FRAME RATE", {u"Images par seconde max.", u"Max. Bildrate", u"Frame rate massimo", u"Fotogramas por segundo máx."}},
    {"FPS COUNTER", {u"Compteur d'images par seconde", u"FPS-Anzeige", u"Contatore FPS", u"Contador de FPS"}},
    {"ANISOTROPIC FILTERING", {u"Filtrage anisotropique", u"Anisotropische Filterung", u"Filtro anisotropico", u"Filtro anisotrópico"}},
    {"DRAW DISTANCE", {u"Distance d'affichage", u"Sichtweite", u"Distanza visiva", u"Distancia de dibujado"}},
    {"AMBIENT OCCLUSION", {u"Occlusion ambiante", u"Ambient Occlusion", u"Occlusione ambientale", u"Oclusión ambiental"}},
    {"RELOAD BLUR", {u"Flou au rechargement", u"Nachlade-Unschärfe", u"Sfocatura in ricarica", u"Desenfoque al recargar"}},
    {"SKIP INTROS", {u"Passer les intros", u"Intros überspringen", u"Salta introduzione", u"Saltar introducción"}},
    {"PAUSE ON FOCUS LOSS", {u"Pause en arrière-plan", u"Pause im Hintergrund", u"Pausa in background", u"Pausa en segundo plano"}},
    {"LANGUAGE", {u"Langue", u"Sprache", u"Lingua", u"Idioma"}},
    {"SUBTITLES", {u"Sous-titres", u"Untertitel", u"Sottotitoli", u"Subtítulos", u"Legendas", u"Teks", u"СУБТИТРЫ"}},
    {"UPDATE TRANSLATIONS", {u"Mettre à jour les traductions", u"Übersetzungen aktualisieren", u"Aggiorna traduzioni", u"Actualizar traducciones", u"Atualizar traduções", u"PERBARUI TERJEMAHAN", u"ОБНОВИТЬ ПЕРЕВОДЫ"}},
    {"CHECK NOW", {u"Vérifier", u"Jetzt prüfen", u"Controlla ora", u"Comprobar ahora", u"Verificar agora", u"PERIKSA SEKARANG", u"ПРОВЕРИТЬ СЕЙЧАС"}},
    {"QUEUED...", {u"En attente...", u"Warteschlange...", u"In coda...", u"En cola...", u"Na fila...", u"ANTREAN...", u"В ОЧЕРЕДИ..."}},
    {"CHECKING TRANSLATIONS...", {u"Vérification des traductions...", u"Übersetzungen werden geprüft...", u"Controllo traduzioni...", u"Comprobando traducciones...", u"Verificando traduções...", u"MEMERIKSA TERJEMAHAN...", u"ПРОВЕРКА ПЕРЕВОДОВ..."}},
    {"TRANSLATIONS UPDATED", {u"Mis à jour", u"Aktualisiert", u"Aggiornato", u"Actualizado", u"ATUALIZADO", u"DIPERBARUI", u"ОБНОВЛЕНО"}},
    {"TRANSLATIONS UP TO DATE", {u"À jour", u"Aktuell", u"In regola", u"Al día", u"EM DIA", u"TERKINI", u"АКТУАЛЬНО"}},
    {"NO LANGUAGE PACKAGES INSTALLED", {u"Aucun paquet linguistique installé", u"Keine Sprachpakete installiert", u"Nessun pacchetto lingua installato", u"No hay paquetes de idioma instalados", u"Nenhum pacote de idioma instalado", u"BELUM ADA PAKET BAHASA", u"ЯЗЫКОВЫЕ ПАКЕТЫ НЕ УСТАНОВЛЕНЫ"}},
    {"UPDATE FAILED (SEE LOG)", {u"Échec de la mise à jour (voir le journal)", u"Aktualisierung fehlgeschlagen (siehe Protokoll)", u"Aggiornamento non riuscito (vedi log)", u"Error al actualizar (ver registro)", u"Falha ao atualizar (veja o log)", u"PEMBARUAN GAGAL (LIHAT LOG)", u"ОШИБКА ОБНОВЛЕНИЯ (СМ. ЖУРНАЛ)"}},
    {"SUBTITLE SETTINGS", {u"Réglages des sous-titres", u"Untertiteleinstellungen", u"Impostazioni sottotitoli", u"Ajustes de subtítulos", u"Configurações da legenda", u"PENGATURAN SUBTITLE", u"НАСТРОЙКИ СУБТИТРОВ"}},
    {"SUBTITLE SIZE", {u"Taille des sous-titres", u"Untertitelgröße", u"Dimensione sottotitoli", u"Tamaño de subtítulos", u"Tamanho da legenda", u"UKURAN SUBTITLE", u"РАЗМЕР СУБТИТРОВ"}},
    {"CURRENT SIZE", {u"Taille actuelle", u"Aktuelle Größe", u"Dimensione attuale", u"Tamaño actual", u"Tamanho atual", u"UKURAN SAAT INI", u"ТЕКУЩИЙ РАЗМЕР"}},
    {"PREVIEW", {u"Aperçu", u"Vorschau", u"Anteprima", u"Vista previa", u"Prévia", u"PRATINJAU", u"ПРЕДПРОСМОТР"}},
    {"This is a subtitle preview.", {u"Voici un aperçu des sous-titres.", u"So sehen die Untertitel aus.", u"Ecco un'anteprima dei sottotitoli.", u"Así se verán los subtítulos.", u"Esta é uma prévia da legenda.", u"Ini contoh tampilan subtitle.", u"Так будут выглядеть субтитры."}},
    {"BOLD", {u"Gras", u"Fett", u"Grassetto", u"Negrita", u"Negrito", u"TEBAL", u"ЖИРНЫЙ"}},
    {"COLOR", {u"Couleur", u"Farbe", u"Colore", u"Color", u"Cor", u"WARNA", u"ЦВЕТ"}},
    {"SHADOW", {u"Ombre", u"Schatten", u"Ombra", u"Sombra", u"Sombra", u"BAYANGAN", u"ТЕНЬ"}},
    {"BACKGROUND", {u"Arrière-plan", u"Hintergrund", u"Sfondo", u"Fondo", u"Fundo", u"LATAR BELAKANG", u"ФОН"}},
    {"WHITE", {u"Blanc", u"Weiß", u"Bianco", u"Blanco", u"Branco", u"PUTIH", u"БЕЛЫЙ"}},
    {"YELLOW", {u"Jaune", u"Gelb", u"Giallo", u"Amarillo", u"Amarelo", u"KUNING", u"ЖЕЛТЫЙ"}},
    {"SUBTLE", {u"Discrète", u"Dezent", u"Leggera", u"Suave", u"Suave", u"TIPIS", u"СЛАБАЯ"}},
    {"STRONG", {u"Forte", u"Stark", u"Forte", u"Fuerte", u"Forte", u"KUAT", u"СИЛЬНАЯ"}},
    {"READY", {u"Prêt", u"Bereit", u"Pronto", u"Listo", u"Pronto", u"SIAP", u"ГОТОВО"}},
    {"MANIFEST NEEDED", {u"Manifeste requis", u"Manifest nötig", u"Manifesto necessario", u"Se necesita manifiesto", u"Manifesto necessário", u"MANIFEST DIPERLUKAN", u"НУЖЕН МАНИФЕСТ"}},
    {"NOT IN MANIFEST", {u"Absent du manifeste", u"Nicht im Manifest", u"Non nel manifesto", u"No está en el manifiesto", u"Não consta no manifesto", u"TIDAK ADA DI MANIFEST", u"НЕТ В МАНИФЕСТЕ"}},
    {"MISSING", {u"Manquant", u"Fehlt", u"Mancante", u"Falta", u"Faltando", u"HILANG", u"ОТСУТСТВУЕТ"}},
    {"NOT DOWNLOADED", {u"Non téléchargé", u"Nicht heruntergeladen", u"Non scaricato", u"No descargado", u"Não baixado", u"BELUM DIUNDUH", u"НЕ ЗАГРУЖЕНО"}},
    {"DAMAGED", {u"Endommagé", u"Beschädigt", u"Danneggiato", u"Dañado", u"Danificado", u"RUSAK", u"ПОВРЕЖДЕНО"}},
    {"CURRENT", {u"Actuel", u"Aktuell", u"Attuale", u"Actual", u"Atual", u"SAAT INI", u"ТЕКУЩИЙ"}},
    {"PENDING", {u"En attente", u"Ausstehend", u"In attesa", u"Pendiente", u"Pendente", u"MENUNGGU", u"ОЖИДАНИЕ"}},
    {"QUEUED", {u"En file", u"Warteschlange", u"In coda", u"En cola", u"Na fila", u"ANTREAN", u"В ОЧЕРЕДИ"}},
    {"DOWNLOADING", {u"Téléchargement", u"Wird geladen", u"Download", u"Descargando", u"Baixando", u"MENGUNDUH", u"ЗАГРУЗКА"}},
    {"OPEN", {u"Ouvrir", u"Öffnen", u"Apri", u"Abrir", u"Abrir", u"BUKA", u"ОТКРЫТЬ"}},
    {"Windows display language not available", {u"Langue d'affichage Windows indisponible", u"Windows-Anzeigesprache nicht verfügbar", u"Lingua di visualizzazione Windows non disponibile", u"Idioma de Windows no disponible", u"Idioma de exibição do Windows indisponível", u"Bahasa tampilan Windows tidak tersedia", u"Язык интерфейса Windows недоступен"}},
    {"UNSUPPORTED WINDOWS LANGUAGE", {u"LANGUE WINDOWS NON PRISE EN CHARGE", u"NICHT UNTERSTÜTZTE WINDOWS-SPRACHE", u"LINGUA WINDOWS NON SUPPORTATA", u"IDIOMA DE WINDOWS NO COMPATIBLE", u"IDIOMA DO WINDOWS NÃO COMPATÍVEL", u"BAHASA WINDOWS TIDAK DIDUKUNG", u"НЕПОДДЕРЖИВАЕМЫЙ ЯЗЫК WINDOWS"}},
    {"LANGUAGE PACKAGE FILES", {u"FICHIERS DU PAQUET LINGUISTIQUE", u"SPRACHPACKETDATEIEN", u"FILE DEL PACCHETTO LINGUISTICO", u"ARCHIVOS DEL PAQUETE DE IDIOMA", u"ARQUIVOS DO PACOTE DE IDIOMA", u"FILE PAKET BAHASA", u"ФАЙЛЫ ЯЗЫКОВОГО ПАКЕТА"}},
    {"The game will use English. Review a suggestion on GitHub?", {u"Le jeu utilisera l'anglais. Proposer cette langue sur GitHub ?", u"Das Spiel verwendet Englisch. Sprache auf GitHub vorschlagen?", u"Il gioco userà l'inglese. Suggerire la lingua su GitHub?", u"El juego usará inglés. ¿Sugerir este idioma en GitHub?", u"O jogo usará inglês. Sugerir esse idioma no GitHub?", u"Game akan menggunakan bahasa Inggris. Sarankan bahasa ini di GitHub?", u"Игра будет использовать английский. Предложить язык на GitHub?"}},
    {"Review on GitHub", {u"Vérifier sur GitHub", u"Auf GitHub prüfen", u"Controlla su GitHub", u"Revisar en GitHub", u"Revisar no GitHub", u"TINJAU DI GITHUB", u"ПРОВЕРИТЬ НА GITHUB"}},
    {"Not now", {u"Pas maintenant", u"Nicht jetzt", u"Non ora", u"Ahora no", u"Agora não", u"JANGAN SEKARANG", u"НЕ СЕЙЧАС"}},
    {"Could not open the browser.", {u"Impossible d'ouvrir le navigateur.", u"Browser konnte nicht geöffnet werden.", u"Impossibile aprire il browser.", u"No se pudo abrir el navegador.", u"Não foi possível abrir o navegador.", u"Browser tidak dapat dibuka.", u"Не удалось открыть браузер."}},
    {"Package status", {u"État du paquet", u"Paketstatus", u"Stato del pacchetto", u"Estado del paquete", u"Estado do pacote", u"Status paket", u"СОСТОЯНИЕ ПАКЕТА"}},
    {"Download in progress.", {u"Téléchargement en cours.", u"Download läuft.", u"Download in corso.", u"Descarga en curso.", u"Download em andamento.", u"Unduhan sedang berlangsung.", u"ЗАГРУЗКА ВЫПОЛНЯЕТСЯ."}},
    {"Download / repair", {u"Télécharger / réparer", u"Herunterladen / reparieren", u"Scarica / ripara", u"Descargar / reparar", u"Baixar / reparar", u"UNDUH / PERBAIKI", u"СКАЧАТЬ / ИСПРАВИТЬ"}},
    {"Close", {u"Fermer", u"Schließen", u"Chiudi", u"Cerrar", u"Fechar", u"TUTUP", u"ЗАКРЫТЬ"}},
    {"All files listed in the manifest are present and match their SHA-256 checks.", {u"Tous les fichiers du manifeste sont présents et conformes aux contrôles SHA-256.", u"Alle Manifestdateien sind vorhanden und stimmen mit den SHA-256-Prüfungen überein.", u"Tutti i file del manifest sono presenti e corrispondono ai controlli SHA-256.", u"Todos los archivos del manifiesto están presentes y coinciden con SHA-256.", u"Todos os arquivos do manifesto estão presentes e conferem com o SHA-256.", u"Semua file dalam manifest tersedia dan cocok dengan SHA-256.", u"Все файлы манифеста на месте и соответствуют проверке SHA-256."}},
    {"Nothing is submitted automatically; you can review the draft before submitting.", {u"Rien n'est envoyé automatiquement ; vérifiez le brouillon avant l'envoi.", u"Es wird nichts automatisch gesendet; prüfe den Entwurf vor dem Absenden.", u"Non viene inviato nulla automaticamente; controlla la bozza prima di inviarla.", u"No se envía nada automáticamente; revisa el borrador antes de enviarlo.", u"Nada é enviado automaticamente; revise o rascunho antes de enviar.", u"Tidak ada yang dikirim otomatis; tinjau draf sebelum mengirim.", u"Ничего не отправляется автоматически; проверьте черновик перед отправкой."}},
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
    {"PORTUGUESE", {u"Portugais", u"Portugiesisch", u"Portoghese", u"Portugués"}},
    {"INDONESIAN", {u"Indonésien", u"Indonesisch", u"Indonesiano", u"Indonesio"}},
    {"RUSSIAN", {u"Russe", u"Russisch", u"Russo", u"Ruso"}},

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

std::atomic<bool> g_russian_font_available{false};
bool JsonField(std::string_view line, std::string_view wanted, std::string& value);

std::string GameMenuTranslation(uint32_t id) {
    const std::string& language = game::BootLanguage();
    if (language == "int") return {};
    std::string locale = language == "esp" ? "es" : language == "fra" ? "fr" :
                         language == "deu" ? "de" : language == "ita" ? "it" : language;
    static std::mutex lock;
    static std::map<std::string, std::map<uint32_t, std::string>> caches;
    static std::map<std::string, std::chrono::steady_clock::time_point> last_attempt;
    static std::map<std::string, bool> missing_logged;
    std::lock_guard guard(lock);
    auto& strings = caches[locale];
    const auto now = std::chrono::steady_clock::now();
    auto& attempted_at = last_attempt[locale];
    if (strings.empty() && (attempted_at.time_since_epoch().count() == 0 ||
                            now - attempted_at >= std::chrono::seconds(1))) {
        attempted_at = now;
        auto path = app::ExeDir() / "language" / locale / "Localization" / "Menu" / (locale + ".jsonl");
        std::ifstream file(path, std::ios::binary);
        std::string line;
        while (std::getline(file, line)) {
            std::string key, value;
            if (!JsonField(line, "id", key) || !JsonField(line, "text", value)) continue;
            uint32_t parsed = 0;
            try { parsed = uint32_t(std::stoul(key, nullptr, 16)); } catch (...) { continue; }
            if (!value.empty()) strings[parsed] = std::move(value);
        }
        if (file.is_open())
            LOGI("translations: loaded %zu original game menu strings for %s", strings.size(), locale.c_str());
        if (strings.empty() && !missing_logged[locale]) {
            LOGW("translations: original game menu catalog is missing for %s; using game defaults", locale.c_str());
            missing_logged[locale] = true;
        }
    }
    auto found = strings.find(id);
    return found == strings.end() ? std::string() : found->second;
}

void AppendUtf8Codepoint(std::string& out, uint32_t codepoint) {
    if (codepoint < 0x80) {
        out += char(codepoint);
    } else if (codepoint < 0x800) {
        out += char(0xC0 | (codepoint >> 6));
        out += char(0x80 | (codepoint & 0x3F));
    } else if (codepoint < 0x10000) {
        out += char(0xE0 | (codepoint >> 12));
        out += char(0x80 | ((codepoint >> 6) & 0x3F));
        out += char(0x80 | (codepoint & 0x3F));
    } else {
        out += char(0xF0 | (codepoint >> 18));
        out += char(0x80 | ((codepoint >> 12) & 0x3F));
        out += char(0x80 | ((codepoint >> 6) & 0x3F));
        out += char(0x80 | (codepoint & 0x3F));
    }
}

bool JsonString(std::string_view line, size_t& at, std::string& out) {
    if (at >= line.size() || line[at++] != '"') return false;
    out.clear();
    while (at < line.size()) {
        unsigned char c = line[at++];
        if (c == '"') return true;
        if (c < 0x20) return false;
        if (c != '\\') {
            out += char(c);
            continue;
        }
        if (at >= line.size()) return false;
        switch (line[at++]) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                auto hex = [&]() -> int {
                    if (at + 4 > line.size()) return -1;
                    uint32_t value = 0;
                    for (int i = 0; i < 4; i++) {
                        char digit = line[at++];
                        value <<= 4;
                        if (digit >= '0' && digit <= '9') value |= uint32_t(digit - '0');
                        else if (digit >= 'a' && digit <= 'f') value |= uint32_t(digit - 'a' + 10);
                        else if (digit >= 'A' && digit <= 'F') value |= uint32_t(digit - 'A' + 10);
                        else return -1;
                    }
                    return int(value);
                };
                int first = hex();
                if (first < 0) return false;
                uint32_t codepoint = uint32_t(first);
                if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
                    if (at + 2 > line.size() || line[at++] != '\\' || line[at++] != 'u') return false;
                    int second = hex();
                    if (second < 0xDC00 || second > 0xDFFF) return false;
                    codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (uint32_t(second) - 0xDC00);
                } else if (codepoint >= 0xDC00 && codepoint <= 0xDFFF) {
                    return false;
                }
                AppendUtf8Codepoint(out, codepoint);
                break;
            }
            default: return false;
        }
    }
    return false;
}

bool JsonField(std::string_view line, std::string_view wanted, std::string& value) {
    size_t at = 0;
    while (at < line.size()) {
        while (at < line.size() && (std::isspace(uint8_t(line[at])) || line[at] == '{' || line[at] == ',')) at++;
        if (at >= line.size() || line[at] == '}') break;
        std::string key, field;
        if (!JsonString(line, at, key)) return false;
        while (at < line.size() && std::isspace(uint8_t(line[at]))) at++;
        if (at >= line.size() || line[at++] != ':') return false;
        while (at < line.size() && std::isspace(uint8_t(line[at]))) at++;
        if (!JsonString(line, at, field)) return false;
        if (key == wanted) {
            value = std::move(field);
            return true;
        }
    }
    return false;
}

std::u16string Utf16(std::string_view text) {
    std::u16string out;
    for (size_t i = 0; i < text.size();) {
        uint8_t first = uint8_t(text[i++]);
        uint32_t codepoint = first;
        int extra = 0;
        if ((first & 0xE0) == 0xC0) { codepoint = first & 0x1F; extra = 1; }
        else if ((first & 0xF0) == 0xE0) { codepoint = first & 0x0F; extra = 2; }
        else if ((first & 0xF8) == 0xF0) { codepoint = first & 0x07; extra = 3; }
        else if (first >= 0x80) { out += u'\uFFFD'; continue; }
        if (i + size_t(extra) > text.size()) { out += u'\uFFFD'; break; }
        bool valid = true;
        for (int n = 0; n < extra; n++) {
            uint8_t next = uint8_t(text[i++]);
            if ((next & 0xC0) != 0x80) { valid = false; break; }
            codepoint = (codepoint << 6) | (next & 0x3F);
        }
        if (!valid || codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
            out += u'\uFFFD';
        } else if (codepoint < 0x10000) {
            out += char16_t(codepoint);
        } else {
            codepoint -= 0x10000;
            out += char16_t(0xD800 + (codepoint >> 10));
            out += char16_t(0xDC00 + (codepoint & 0x3FF));
        }
    }
    return out;
}

std::string UpperAscii(std::string_view text) {
    std::string key(text);
    std::transform(key.begin(), key.end(), key.begin(), [](char c) { return char(std::toupper(uint8_t(c))); });
    return key;
}

const std::map<std::string, std::u16string>& JsonTranslations() {
    static std::map<std::string, std::u16string> translations;
    static auto last_attempt = std::chrono::steady_clock::time_point{};
    const std::string& language = game::BootLanguage();
    if (language != "pt-BR" && language != "id" && language != "ru") return translations;
    const auto now = std::chrono::steady_clock::now();
    if (translations.empty() && now - last_attempt >= std::chrono::seconds(1)) {
        last_attempt = now;
        auto path = app::ExeDir() / "language" / language / "Localization" / "Menu" / "recomp.jsonl";
        if (!std::filesystem::exists(path))
            path = app::ExeDir() / "language" / "recomp-menu" / (language + ".jsonl");  // compatibility with the first test package
        std::ifstream file(path, std::ios::binary);
        std::string line;
        while (std::getline(file, line)) {
            if (line.size() >= 3 && uint8_t(line[0]) == 0xEF && uint8_t(line[1]) == 0xBB && uint8_t(line[2]) == 0xBF)
                line.erase(0, 3);
            std::string key, value;
            if (JsonField(line, "key", key) && JsonField(line, "text", value) && !key.empty() && !value.empty())
                translations[UpperAscii(key)] = Utf16(value);
        }
        if (!translations.empty()) LOGI("translations: loaded %zu Recomp UI strings for %s", translations.size(), language.c_str());
    }
    return translations;
}

// the language the game booted in, -1 for English
int Language() {
    static const int language = [] {
        auto at = std::find(std::begin(game::kLanguages), std::end(game::kLanguages), game::BootLanguage());
        return at == std::end(game::kLanguages) ? -1 : int(at - std::begin(game::kLanguages)) - 1;
    }();
    return language;
}

const char16_t* Translation(std::string_view english) {
    const int language = Language();
    if (language == 6 && !g_russian_font_available.load(std::memory_order_relaxed)) return nullptr;
    if (language >= 4) {
        const auto& translations = JsonTranslations();
        auto it = translations.find(UpperAscii(english));
        if (it != translations.end()) return it->second.c_str();
    }
    if (language < 0 || language >= 7) return nullptr;
    for (const Entry& e : kEntries)
        if (Same(e.english, english) && !e.text[language].empty()) return e.text[language].data();
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
    if (c >= 0x0430 && c <= 0x044F) return char16_t(c - 0x20);
    if (c == 0x0451) return 0x0401;
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

void app::SetRussianFontAvailable(bool available) {
    g_russian_font_available.store(available, std::memory_order_relaxed);
}

std::string app::GameMenuText(uint32_t id) { return GameMenuTranslation(id); }

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
