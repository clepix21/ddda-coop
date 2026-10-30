<p align="center"><a href="README.md">English</a> · <b>Français</b></p>

<p align="center">
  <img src="assets/readme/fr/hero.svg" width="100%" alt="DDDA Coop : joue en ligne à Dragon's Dogma: Dark Arisen avec 1 à 3 amis, chacun avec son propre perso à la place d'un pion de l'hôte">
</p>

<p align="center">
  <a href="https://github.com/clepix21/ddda-coop/releases/latest"><b>Télécharger</b></a> ·
  <a href="#installation">Installation</a> ·
  <a href="#jouer">Jouer</a> ·
  <a href="#bon-à-savoir">Bon à savoir</a> ·
  <a href="docs/TECHNICAL.md">Doc technique (EN)</a>
  <br>
  <sub>Alpha · Windows · Steam · launcher en français, English, italiano, Deutsch, español</sub>
</p>

<p align="center">
  <img src="assets/readme/fr/launcher.png" width="100%" alt="La fenêtre DDDA Coop : l'écran Jouer avec les rôles Hôte, Invité et Seul, et le salon avec les amis connectés, leur niveau et leur ping">
</p>

<!-- Captures en jeu : remplace assets/readme/fr/ingame-1.svg (et en/ pour la version anglaise), -2 et -3 par tes captures (garde le même nom
     en .png ou .jpg, et change l'extension ci-dessous). Format conseillé : 16:9, 1280 px de large. -->
<p align="center">
  <img src="assets/readme/fr/ingame-1.svg" width="32%" alt="Capture en jeu à venir : combattre ensemble">
  <img src="assets/readme/fr/ingame-2.svg" width="32%" alt="Capture en jeu à venir : relever un ami">
  <img src="assets/readme/fr/ingame-3.svg" width="32%" alt="Capture en jeu à venir : chacun son perso">
</p>

## En bref

L'hôte joue sa partie comme d'habitude. Jusqu'à trois amis Steam le rejoignent, chacun sur son PC et
avec **son propre perso** (apparence, vocation, niveau, équipement) qui prend la place d'un pion.
Monstres, butin, quêtes et expérience sont partagés. Ce que ton perso gagne chez un ami revient dans ta
partie solo.

## Comment ça marche

<p align="center">
  <img src="assets/readme/fr/how-it-works.svg" width="100%" alt="Le PC de l'hôte garde le monde de référence. Par Steam, chaque ami reçoit une copie de ce monde et y joue son propre perso. Positions, actions et compétences vont dans les deux sens ; les monstres viennent de l'hôte ; butin, quêtes et expérience sont partagés ; un joueur à terre peut être relevé.">
</p>

Seul l'hôte sauvegarde : la partie d'un invité n'est jamais écrasée. Chaque joueur peut aussi partir
explorer de son côté et revenir quand il veut.

## Installation

1. Télécharge `DDDA-Coop-vX.Y.Z.zip` dans les [Releases](https://github.com/clepix21/ddda-coop/releases)
   et dézippe-le où tu veux.
2. Ouvre `DDDA-Coop.exe`. Il trouve le jeu tout seul et sauvegarde tes parties avant chaque lancement.
3. Choisis ton rôle (**Hôte**, **Invité** ou **Seul**) puis clique sur **Lancer le jeu**.

Il te faut Dragon's Dogma: Dark Arisen sur **Steam** (build 2364871, la version PC actuelle). Tout le
monde doit avoir **la même version du mod** : la fenêtre te prévient sinon.

## Jouer

**L'hôte** lance le jeu et charge sa partie. Ses amis Steam qui ont le mod le rejoignent tout seuls.

**Un invité** lance le jeu et reste sur l'écran titre jusqu'à ce que la fenêtre affiche
« Connecté ! », puis appuie sur Start et charge une partie : c'est le monde de l'hôte qui arrive.

Plusieurs amis hébergent en même temps ? L'hôte ouvre **Salon** › **Copier le code**, et l'invité colle
ce code dans son propre **Salon** avant de lancer.

Les touches et tous les détails sont dans `LISEZMOI.txt`, à l'intérieur du zip.

## Bon à savoir

- **Après le tutoriel seulement.** La co-op démarre quand l'hôte et chaque invité ont un perso qui a
  fini le tutoriel, c'est-à-dire avec son pion principal. Sur une nouvelle partie, faites-le chacun
  seul d'abord.
- **Alpha.** Testé surtout par des bots et lors d'une soirée entre amis. Garde le « Journal détaillé »
  activé (Paramètres) pour pouvoir signaler un bug.
- Les autres joueurs peuvent encore glisser un peu quand la connexion est irrégulière (amélioré en
  0.9.2).
- Pas encore testés : la Mort (Île du Crépuscule), l'eau profonde, les longues sessions.
- Les fonctions en ligne du jeu (la Faille, les pions d'autres joueurs) ne sont pas prises en charge
  par le mod : évite-les pendant une partie en co-op.

Un bug ? Ouvre une [issue](https://github.com/clepix21/ddda-coop/issues) et joins le journal (écran
**Journal** › **Exporter le journal**).

<details>
<summary><b>Questions fréquentes</b></summary>

**Est-ce que ça touche à mes sauvegardes ?** Avant chaque lancement, la fenêtre copie tes sauvegardes
dans `Documents\DDDA Coop\Sauvegardes`. Le jeu d'un invité ne sauvegarde jamais.

**Comment revenir au jeu normal ?** **Paramètres** › **Retirer le mod**. Tu peux aussi supprimer
`dinput8.dll` et `coop.ini` du dossier du jeu.

**Ça marche hors de Steam ?** Pas pour le moment. Il existe un mode réseau local pour les tests (voir la
doc technique).

</details>

<details>
<summary><b>Compiler depuis les sources</b></summary>

Il faut Windows, [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) et
[MinHook](https://github.com/TsudaKageyu/minhook) 1.3.4 :

```powershell
.\build.ps1 -Toolchain <dossier llvm-mingw> -MinHook <dossier minhook>
```

La compilation donne `build\dinput8.dll` (le mod), `build\DDDA-Coop.exe` (la fenêtre) et
`build\DDDA-Coop.zip`. Le fonctionnement et les tests automatiques sont décrits dans
[docs/TECHNICAL.md](docs/TECHNICAL.md).

</details>

## Crédits

- **clepix21** : auteur du mod.
- **GrosGadjo** et **yaybba** : testeurs.
- Développé avec l'aide de **Claude** (Anthropic).
- [MinHook](https://github.com/TsudaKageyu/minhook) (licence BSD 2-Clause) et la police
  [Inter](https://github.com/rsms/inter) (SIL Open Font License 1.1) : leurs licences sont dans
  [dist/LICENCES.txt](dist/LICENCES.txt).

<sub>Mod non officiel, sans lien avec Capcom. Dragon's Dogma: Dark Arisen © Capcom. Il te faut ta propre
copie du jeu : ce dépôt ne contient aucun fichier du jeu.</sub>
