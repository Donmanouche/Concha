# Concha — Strudel en plugin CLAP/VST3 (plan d'implémentation)

## Contexte

Strudel (codeberg.org/uzu/strudel, AGPL-3.0) est un live-coding de patterns en JavaScript qui tourne dans le navigateur. Le son passe par superdough (WebAudio), et SuperDirt n'est joignable qu'à travers un pont Node websocket. Le MIDI passe par WebMIDI. Concha le transforme en plugin **CLAP + VST3** (Linux d'abord, puis Windows et Mac), qui doit :

- reprendre tout le langage de Strudel ;
- lire les samples locaux sans aucun serveur, avec des dossiers nommés ;
- avoir 16 sorties stéréo, où orbit = sortie ;
- envoyer du MIDI vers le DAW (notes, CC, Program Change, bend, NRPN, sysex) ;
- se synchroniser exactement sur le transport ;
- embarquer scsynth pour les SynthDefs SuperDirt ;
- avoir plus tard des visuels GPU.

**Décisions prises avec toi :**

| Sujet | Choix |
|---|---|
| Moteur son | Hybride : superdough réécrit en C++ (« superdough C++ », décision confirmée après la phase 0) + scsynth compilé dans le plugin. Rien d'autre à installer. Le `dough` de Strudel ne sert que de source de code réutilisable, et éventuellement de mode `.dough()`. |
| Framework | JUCE 8 (AGPLv3) + clap-juce-extensions, pour du code portable |
| Interface | Native, JUCE + OpenGL |
| Hôtes à valider | Bitwig Studio, Carla. Audio et MIDI passent par l'hôte, sans routage JACK. Le standalone utilisera PipeWire. |
| Synchro | 1 cycle = N temps (N=4 par défaut). La position suit le DAW. Le plugin se tait à l'arrêt, avec un bouton « Free run ». |
| Sorties | 16 stéréo : l'orbit k va sur la k-ième paire stéréo, comme superdough en multicanal (orbits 0 et 1 → Main, orbit 2 → « Orbit 2 »… orbit 16), chaque orbit a sa réverb et son delay. `.channels()` choisit la sortie sans changer l'orbit (précisé en phase 3 ; remplace l'idée d'un `.out(n)`). |
| MIDI in | Notes et CC utilisables dans les patterns, plus 16 macros automatisables |
| Modules | Tous dans la v1 : soundfonts GM, Csound, Mondo, Tidal (hs2js), gamepad, MQTT, port série. Hydra sera remplacé par les visuels natifs. `motion`, `web`, `repl`, `codemirror` et `desktopbridge` n'ont pas de sens dans un plugin. |
| Samples | Dossiers locaux nommés, **tous ajoutés par l'utilisateur : aucun dossier par défaut**. Cache disque pour `github:`/URL. |
| Licence | AGPL-3.0, pour un usage personnel ou libre |

**Environnement constaté :**

- Ubuntu **24.04** (et non 22.04), g++ 13.3, cmake 3.28, node 20, pnpm.
- Paquets de développement présents : GTK3, X11, GL, ALSA, JACK, freetype, libcurl, sndfile.
- SDK VST3 dans `~/vst3sdk`.
- SuperCollider 3.13 et sc3-plugins, avec SuperDirt et Dirt-Samples dans les quarks.
- `../deps` contient déjà dr_libs (wav, flac, mp3) et stb_vorbis.
- **Absents :** JUCE 8 (seulement 7.0.5 dans le système et dans Openbridge ; les sous-modules d'OB-Xf sont vides), les en-têtes CLAP, QuickJS, Csound, SDL3, libserialport.

## Architecture

```
 Fil UI (JUCE/OpenGL)        Fil Pattern (non temps réel)             Fil Audio (temps réel, callback de l'hôte)
 éditeur, console,  ──code──▶ QuickJS-ng + bundle Strudel JS  ──haps──▶ Scheduler : cycle → offset en samples
 bibliothèque,              (core, mini, transpiler, tonal,  (file      ├─ superdough C++ (synthés, sampler, FX)    
 visuels ◀──haps actives──  xen, mondo, tidal, contrôles…)   lock-free) ├─ scsynth embarqué (World, driver « hôte »)
                            interroge [t, t+lookahead]                 ├─ Sortie MIDI → hôte (offset au sample près)
                            ◀──instantané du transport (ppq, BPM)──    └─ 16 bus stéréo → sorties du plugin
```

- **JS :** on garde Strudel tel quel, épinglé à un commit dans `third_party/strudel`. Un bundle esbuild unique `js/concha-strudel.js` tourne dans **QuickJS-ng**, qui est portable, écrit en C et compact. De petites cales remplacent `fetch`, `setTimeout` et `performance`. Les fonctions liées au navigateur sont réimplémentées en natif sous les mêmes noms : `samples()`, `soundfont`, `.scope()`, `.pianoroll()`, `setcps`, `midi`… Une sortie « concha » remplace `superdough()`. Chaque hap devient un événement POD (temps en cycles + paramètres compacts) avec ses emplacements dans le code, qui servent à surligner les notes jouées.
- **Synchro :** `cps = BPM/60/N` et `cycle = ppq/N`. Le fil audio publie à chaque bloc un instantané du transport (ppq, BPM, lecture). Le fil pattern interroge environ 150 ms en avance. Une discontinuité (boucle, saut, changement de tempo) vide la file et relance l'interrogation. Une ré-évaluation remplace les événements futurs à partir de « maintenant ». Le résultat ne dépend que de la position dans les cycles, donc il est identique d'une lecture à l'autre.
- **superdough C++ :** réécriture de superdough et de ses 17 worklets. Les nœuds WebAudio (biquad, convolution, compresseur, panner) sont recodés en suivant la spécification. Les worklets sont traduits ligne à ligne. On reprend de `dough` (moteur C de Strudel, AGPL) les parties utiles : oscillateurs anti-aliasés, gestion des voix, lecture PCM, chorus, flanger. Le but est le même son que strudel.cc :
  - sampler : begin/end, speed, loop, cut, n ;
  - oscillateurs, supersaw, pulse, bruits, FM, wavetables, zzfx ;
  - ADSR et enveloppes de filtre et de hauteur ;
  - biquads lpf/hpf/bpf/notch, vowel ;
  - distort/shape/crush/coarse, phaser, tremolo, compresseur ;
  - delay, réverb (IR générée) et room par orbit.
- **scsynth embarqué :** compilé depuis les sources de SuperCollider (bibliothèque statique, UGens de base et sc3-plugins inclus). Un driver audio « hôte » appelle le World par blocs de 64 samples ; la latence est déclarée au DAW. Les commandes OSC sont horodatées en temps-sample. La logique SuperDirt (DirtOrbit/DirtEvent, chaîne d'effets, effets globaux par orbit) est réécrite en C++. Les SynthDefs SuperDirt sont précompilées en `.scsyndef` par un script sclang lancé hors ligne, puis committées. Le moteur est choisi selon le nom : un SynthDef connu ou `.engine('sc')` part vers scsynth, le reste va à Dough. Les buffers des samples sont partagés avec le cache C++.
- **MIDI out :** reprend la sémantique de `@strudel/midi` : note+durée → note on/off, `ccn/ccv`, `progNum`, `midibend`, `miditouch`, `nrpnn/nrpv`, `sysex`, `midichan`. Ça passe par le port de notes CLAP (dialecte MIDI) et par l'event bus VST3 (LegacyMIDICCOut). `midicmd` clock/start/stop est ignoré, puisque le DAW est le maître.
- **Bibliothèque de samples :** la liste est vide au départ. L'utilisateur ajoute ses dossiers et leur donne un nom. Les dossiers nommés sont enregistrés dans les réglages globaux (`~/.config/Concha/settings.json`).
  - Chaque sous-dossier est un son, et les fichiers triés donnent l'index `n` (convention Dirt-Samples). Un `strudel.json` local est aussi accepté.
  - *Proposition modifiable :* `samples('lib:MonAlias')` charge un dossier ; une case « charger au démarrage » le rend disponible directement ; `.bank("MonAlias")` est aussi possible.
  - Décodage avec dr_libs et stb_vorbis, déjà dans `../deps`. Chargement paresseux en arrière-plan, avec préchargement des sons rencontrés dans les prochains cycles.
- **État :** le code, les réglages et les macros sont sauvegardés dans le projet du DAW. Les dossiers et le cache sont globaux.

## Arborescence prévue (`Concha/`)

`CMakeLists.txt`, `CLAUDE.md`, `src/plugin` (processeur JUCE, bus, état), `src/pattern` (QuickJS, cales, conversion des haps), `src/sched`, `src/superdough`, `src/sc` (driver hôte, SuperDirt C++), `src/midi`, `src/library`, `src/ui`, `src/modules` (soundfonts, csound, gamepad, mqtt, serial), `js/` (entrée du bundle + cales), `third_party/`, `resources/synthdefs`, `tools/` (concha_query, concha_render, compile_synthdefs.scd), `tests/`.

## Phases (chacune se termine par un test dans Bitwig ou Carla)

0. **Amorçage et levée des risques.**
   - Cloner Strudel à un commit épinglé et analyser le nouveau paquet `dough`, apparu après ta copie d'août 2025 : il pourrait servir de base directe au DSP. *Résultat : non, il couvre trop peu et sonne différemment (voir PHASE0.md) ; on réécrit superdough.*
   - Prototype : le bundle Strudel tourne dans QuickJS, avec une mesure des performances.
   - Squelette JUCE CLAP+VST3 (16 sorties stéréo, sortie MIDI) qui se charge dans Bitwig et dans Carla.
   - Prototype : libscsynth dans le process, avec deux instances.
1. **Moteur de patterns sans interface.** Outil `concha_query` (code → haps). Les haps sont comparés aux snapshots des morceaux du dépôt Strudel (`test/__snapshots__/tunes…`).
2. **Scheduler, synchro et MIDI out.** Premier résultat audible, avec les instruments du DAW. Éditeur minimal.
3. **superdough C++ v1.** Samples, synthés de base, ADSR, filtres, gain/pan, orbits → 16 sorties. Gestionnaire de dossiers nommés et cache `github:`.
4. **superdough C++ v2.** Parité de tous les effets et synthés, avec des rendus de référence comparés hors ligne (voir Vérification).
5. **Interface complète.**
   - Éditeur OpenGL : coloration JS et mini-notation, surlignage des haps actifs.
   - Complétion et aide tirées de `doc.json`.
   - Console d'erreurs, raccourcis Strudel (Ctrl+Entrée pour évaluer, Ctrl+. pour hush), navigateur de samples, réglages (N, free run), vumètres par orbit.
6. **scsynth embarqué et SuperDirt en C++.** Script de compilation des SynthDefs, SynthDefs personnelles (dossier `.scsyndef`).
7. **Modules.** MIDI in (`midin`, CC → signaux, notes), soundfonts GM (téléchargées une fois puis en cache), Mondo, Tidal, Csound (libcsound embarqué), gamepad (SDL3), MQTT, port série.
8. **Visuels (plus tard).** pianoroll, punchcard, spiral, scope, spectre, pitchwheel en OpenGL. Puis des shaders GLSL personnalisables, alimentés par les haps et l'audio.
9. **Portage Windows et Mac.** Builds CI, AU sur Mac, chemins par défaut propres à chaque OS.

## Téléchargements nécessaires (ta permission sera demandée avant chacun, rien n'est installé pendant la planification)

- Rangés dans `../deps` (partagés) :
  - JUCE 8 (dernière version 8.0.x) ;
  - clap-juce-extensions (+ clap, clap-helpers) ;
  - les sources de SuperCollider (même version que celle installée) et de sc3-plugins ;
  - les sources de Csound.
- Rangés dans `Concha/third_party` ou récupérés par FetchContent :
  - le clone de Strudel (codeberg) ;
  - QuickJS-ng ;
  - SDL3 (gamepad) ;
  - libserialport ;
  - un client MQTT léger (ou une implémentation maison) ;
  - doctest.
- Outils de développement seulement (npm, dans `Concha/js/node_modules`) :
  - esbuild ;
  - node-web-audio-api, pour les rendus de référence ;
  - plus tard clap-validator et pluginval.
- Aucun paquet système (apt) n'est prévu. Les SynthDefs SuperDirt viennent du quark déjà installé. Elles sont compilées une fois au build, sans aucun sample par défaut.

## Risques à lever en phase 0

- Performances de QuickJS sur les patterns lourds. On interroge en avance pour compenser, et V8 reste une option de repli.
- Plusieurs Worlds scsynth dans un même process (état global de SC).
- CC et Program Change en sortie VST3 dans Bitwig. CLAP est plus sûr, donc c'est le format à privilégier dans Bitwig.
- Fidélité au son WebAudio.
- Évolution de Strudel en amont : commit épinglé et script de mise à jour qui relance les tests de haps.

## Vérification

- `tests/` (doctest) : conversion cycle → sample, files lock-free, scan de la bibliothèque, traduction MIDI, DSP de superdough C++ (réponses de filtre, ADSR).
- `concha_query` comparé aux snapshots de Strudel : mêmes haps pour tous les morceaux de test.
- `concha_render` (rendu hors ligne) comparé à node-web-audio-api qui joue le même code avec superdough : écart de spectre et d'enveloppe sous un seuil.
- clap-validator et pluginval sur les deux formats.
- Bitwig :
  - Boucle de 4 mesures, changement de tempo automatisé, saut de position : les haps restent sur la grille.
  - Enregistrer la sortie MIDI dans un clip : notes, CC et Program Change au bon tick.
  - Router `.orbit(0..15)` vers 16 pistes.
  - Deux instances en parallèle.
  - Charger et recharger le projet.
- Carla : les mêmes contrôles de base, et un test de charge CPU.
