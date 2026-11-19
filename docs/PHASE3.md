# Phase 3 — superdough C++ v1 et bibliothèque de samples (16 novembre 2026)

Objectif ([PLAN.md](PLAN.md)) : remplacer la pré-écoute par le vrai moteur de superdough réécrit en C++ (sources, enveloppes, filtres, panoramique, orbits vers les 16 sorties) et permettre de jouer ses propres dossiers de samples.

## Ce qui existe maintenant

| Pièce | Fichier | Rôle |
|---|---|---|
| Registre des sons (JS) | `js/src/sounds.mjs` | Reprend le `soundMap` de superdough : `samples()` (objet, URL, `github:`, `bubo:`, `shabda:`, et `'Nom'` / `'lib:Nom'` pour les dossiers de la bibliothèque), `aliasBank`, `soundAlias`, `.bank()`, les noms des synthés. À la requête, chaque hap est résolu en son de synthé ou en fichier, avec `getCommonSampleInfo` de superdough tel quel (index `n`, note la plus proche pour les banques à notes), et sa fréquence est calculée selon `getFrequencyFromValue`. Un son inconnu est sauté avec le même message que superdough. |
| Paramètres | `src/superdough/SoundParams.h`, `src/sched/SoundConvert` | Un événement son porte tous les contrôles lus par superdough. NaN veut dire « non défini », comme `undefined` en JS, ce qui permet de reprendre les `??` tels quels. **Un contrôle pas encore implémenté (room, delay…) est signalé une fois dans la console.** |
| WebAudio | `src/superdough/AudioParam.h`, `Nodes` | `AudioParam` avec les automations de la spec (`setValueAtTime`, rampes linéaires et exponentielles avec les cas limites de la spec, `setTargetAtTime`). Oscillateurs à tables limitées en bande, faits comme dans Chromium (3 tables par octave, normalisées sur la forme d'onde complète). Biquad selon les formules de la spec (Q en dB pour passe-bas et passe-haut). Panoramique stéréo à puissance constante. Bruits avec les algorithmes de `noise.mjs`. |
| Moteur | `src/superdough/Engine` | Chaîne de `superdough()` : source → gain×velocity → lpf → hpf → bpf (12 dB, ou 24 dB = deux en série) → pan → `FXrelease` → postgain → orbit. Le code temps réel n'alloue pas de mémoire. Une voix démarre au sample près. |
| Sources | idem | **Synthés** (`synth.mjs`) : `sine`, `square`, `sawtooth`, `triangle` (+ alias), `one`, ADSR (défauts 0,001/0,05/0,6/0,01), gain 0,3, vibrato, enveloppe de hauteur, FM (`fmi`, `fmh`, `fmwave`, enveloppe FM), `noise`. **Bruits** : white, pink, brown, crackle (`density`). **Sampler** (`sampler.mjs`) : `speed` (négatif = à l'envers), `begin`/`end`, `loop`/`loopBegin`/`loopEnd`, `cut`, `clip`, `nudge`, `unit("c")`, vibrato, enveloppe de hauteur, ADSR. La durée suit la longueur de la tranche quand ni `clip`, ni `loop`, ni `release` ne sont donnés. |
| Filtres | idem | `cutoff`/`resonance`, `hcutoff`/`hresonance`, `bandf`/`bandq`, enveloppes (`lpenv`, `lpattack`… défauts 0,005/0,14/0/0,1, exponentielles), `fanchor`, `ftype` 12db/24db. |
| Orbits → sorties | `src/plugin/PluginProcessor` | **Comme superdough en multicanal : l'orbit k va sur la k-ième paire stéréo.** L'orbit 1 (par défaut) et l'orbit 0 vont sur Main, l'orbit 2 sur la sortie « Orbit 2 », jusqu'à l'orbit 16. `.channels()` change la sortie sans changer l'orbit. |
| Polyphonie | idem | 128 voix au plus. Au-delà, les plus anciennes s'éteignent en 0,25 s, comme dans superdough. |
| Bibliothèque | `src/library/SampleLibrary`, `Decoder`, `LibraryHost` | **Aucun dossier par défaut.** Dossiers nommés dans `~/.config/Concha/library.txt`. Lecture des formats WAV, AIFF, FLAC, MP3 et Ogg. Téléchargements `http(s)` mis en cache dans `~/.cache/Concha/http`, donc `github:` n'est téléchargé qu'une fois. Deux fils chargent les fichiers. La bibliothèque est partagée par toutes les instances du plugin, donc un sample n'est chargé qu'une fois. Les samples des deux prochains cycles commencent à charger juste après l'évaluation, et le nouveau code attend 250 ms au plus qu'ils soient prêts. |
| Organisation des dossiers | `SampleLibrary::scanFolder` | (Revu le 19 novembre 2026.) `<Nom>` est le nom que tu donnes au dossier.<ul><li>Un `strudel.json` à la racine est utilisé tel quel.</li><li>Les fichiers à la racine : `<Nom>` (avec `n` pour choisir le fichier) et chaque fichier sous `<Nom>_<fichier>`. Par exemple, `yoyo.wav` dans un dossier nommé `BreaksCool` donne `BreaksCool_yoyo`.</li><li>Tout sous-dossier contenant des fichiers, **à n'importe quelle profondeur**, forme un son à son nom (organisation de Dirt-Samples, `n` choisit le fichier). Il existe aussi sous `<Nom>_<dossier>`, donc `s("yoyo").bank("BreaksCool")` fonctionne. Un nom de dossier rencontré deux fois prend celui de son parent : `<parent>_<dossier>`.</li><li>Une banque, à n'importe quelle profondeur : `Banque/Banque_bd/` ou `Banque/banque-bd/` donne `Banque_bd`, donc `.bank("Banque")` fonctionne (organisation des tidal-drum-machines).</li><li>Le dossier peut être lui-même une banque : `AkaiLinn/akailinn-bd/` donne `AkaiLinn_bd`, avec le nom que tu lui as donné.</li><li>Les noms gardent leur casse dans la liste des sons ; la recherche l'ignore, comme dans Strudel. Les caractères que la mini-notation ne permet pas d'écrire deviennent un seul `_` : `Strings & Things` donne `Strings_Things`.</li></ul>Ajouter, retirer ou renommer un dossier met à jour la liste des sons **sans réévaluer le code** (`PatternWorker::requestLibraryRefresh`), et les sons d'un dossier retiré sont oubliés. |
| Fenêtre | `src/plugin/LibraryPanel` | Bouton **Samples** : ajouter un dossier (avec un nom unique proposé), le renommer, cocher « au démarrage », le retirer de la liste (les fichiers ne sont pas touchés). Un changement recharge la bibliothèque (liste Sounds comprise) sans réévaluer le code. Chaque ligne affiche le nombre de sons trouvés, ou en rouge la raison quand il n'y en a aucun (dossier absent, illisible, sans fichier audio) ; la console écrit `[library] Nom: N sounds` à chaque chargement. La ligne d'état affiche aussi le nombre de voix. |
| Outil | `tools/concha_render` | Code → fichier WAV, par le même chemin que le plugin. Donne le facteur temps réel et le pic. |

La pré-écoute de la phase 2 est supprimée.

## Tests

`ctest` : 3 suites, **40 tests**, toutes vertes.
- `tests/superdough_tests.cpp` (13 tests) :
  - automations conformes à la spec ;
  - règles ADSR de superdough ;
  - gain du passe-bas égal à 10^(Q/20) à la fréquence de coupure ;
  - oscillateurs normalisés et à la bonne hauteur ;
  - panoramique ;
  - voix : départ au sample, durée + release, sortie de l'orbit, niveau de sustain ;
  - enveloppe de filtre ;
  - limite de 128 voix ;
  - sampler : fichier, `begin`, lecture à l'envers, vitesse ×2, groupe `cut` ;
  - organisations de dossiers.
- `tests/pattern_tests.cpp` : un test de résolution des sons en plus.
- La parité avec Strudel est inchangée : 0 différence inexpliquée dans node et dans QuickJS.
- Rendus manuels avec `concha_render` :
  - hauteurs c3, e3, g3, c4 justes, panoramique 0/0,3/0,7/1 correct ;
  - `s("bd*2 [sd hh], hh*8").bank("AkaiLinn")` et `samples('dirt'); s("bd:3 sn:1 …").speed("1 1 -1 2")` joués depuis tes dossiers (dans une configuration de test temporaire : la tienne reste vide).
- **Charge** : 128 voix tenues en permanence, chacune sawtooth avec filtre résonant, enveloppe et panoramique (2048 notes) : rendu 5 fois plus rapide que le temps réel, soit environ 20 % d'un cœur à polyphonie pleine.

## Comparaison avec le vrai superdough

Le vrai superdough (`third_party/strudel/packages/superdough`, sans modification) joue le même code dans node, sur l'`OfflineAudioContext` de `node-web-audio-api` 2.2.0 (installé avec ton accord, dans `js/node_modules`, pour les tests uniquement). Les outils : `js/ref/superdough-render.mjs`, `tests/ref/run_cases.py` et `tests/ref/compare.py`. Les mesures portent sur l'enveloppe par 10 ms, les spectres par tiers d'octave, la balance, la longueur et la corrélation des formes d'onde.

**27 cas sur 27 sont proches de superdough.** Ils couvrent les synthés, l'ADSR, les filtres et leurs enveloppes, le 24 dB, le pan, gain/velocity/postgain, le vibrato, l'enveloppe de hauteur, la FM avec et sans enveloppe, le clip, les trois bruits, et le sampler (fichier, speed, reverse, begin/end, note, loop, cut, stéréo + pan, n, ADSR + clip).
- **Identiques** (corrélation 1,000, écarts à 0,0 dB) : sine, triangle, gains, vibrato, enveloppe de hauteur, FM, et **tout le sampler**, y compris un fichier à 44,1 kHz rééchantillonné à 48 kHz.
- **Sawtooth et square** : même forme d'onde (corrélation 1,000), mais Concha est 1,4 dB plus bas. La référence Rust ne normalise pas ces ondes sur leur pic (le dépassement de Gibbs), alors que la spec WebAudio et Chromium le font, comme Concha. C'est une différence de la référence : Concha garde le comportement de Chromium, donc celui de strudel.cc. Le niveau est compensé dans la comparaison.
- **Bruits** : deux tirages aléatoires indépendants, comparés sur 100 ms et au-dessus de 200 Hz. Les écarts restent sous 1 dB en médiane et sous 2 dB par bande de spectre.

**Corrigé grâce à la comparaison :** dans superdough, l'oscillateur modulateur de la FM démarre au début du contexte audio (`osc.start()` sans heure), pas au début de la note. Sa phase au début de chaque note dépend donc de l'heure absolue. Concha reproduit maintenant cette particularité (corrélation passée de 0,90 à 1,000).

Commandes :
```
node js/ref/build-ref.mjs js/ref/dist/superdough-render.mjs   # une fois, ou après une mise à jour de Strudel
python3 tests/ref/run_cases.py [filtre]
```

## Pas encore là (phase 4)

- **Effets** : `delay`, `room` (réverb), `vowel`, `coarse`, `crush`, `shape`, `distort`, `tremolo`, `compressor`, `phaser`, `djf`, `duckorbit`, `bus`, chaînes `FX`, modulateurs `lfo`/`env`/`bmod`, LFO des filtres (`lprate`…), filtre `ladder`, `stretch`, `transient`.
- **Sources** : `supersaw`, `pulse`, `bytebeat`, `sbd`, `user` (`partials`), wavetables (`wt_…`), `zzfx`, et `K()` (kabelsalat).
- Soundfonts GM : phase 7.

**Comportement à connaître :** un sample qui n'est pas encore chargé au moment où il doit jouer est sauté, comme dans superdough (« took too long »). Ça n'arrive que pour un fichier rencontré pour la première fois loin dans le morceau, ou téléchargé depuis le réseau.

## Ce qui n'a pas encore été vérifié

- **`samples('github:…')` en vrai** : le code et le cache sont écrits, mais je n'ai rien téléchargé sans ton accord.
- **Le plugin dans Bitwig**, avec le panneau Samples.
