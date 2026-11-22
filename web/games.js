// SPDX-License-Identifier: MIT
// The games the converter makes builds of, as the page, the worker and the player name them. The
// id is the converter's (games/<id>/game.toml) and the directory the browser stores the game in.
// `languages` are the releases archived on archive.org (abandonware): each one's medium, which
// the page downloads and checks before converting it. They must match the manifests (a test in
// rc-manifest checks that every medium's SHA-256 is here). The page fetches them through
// archive.org's /cors/ path, the one that answers other sites for every kind of file (/download/
// does so only for text).
const IA = 'https://archive.org/cors/';

export const GAMES = {
  sacked: {
    id: 'sacked',
    name: 'Sacked',
    full: 'Sacked: Your Last Day (Gefeuert! – Dein letzter Tag)',
    stem: 'Sacked',
    size: 'about 250 MiB',
    web: 'sacked',
    icon: 'sacked.svg',
    languages: [
      { code: 'de', label: 'Deutsch', title: 'Gefeuert! – Dein letzter Tag', file: 'sd-gefeg.bin',
        url: `${IA}cdgefrtdlt/sd-gefeg.bin`, size: 143641344,
        sha256: '84d483e65bb506f5d832325815a95e04dc31d985d70dcf8ebaa18aee83c9e93e' },
      { code: 'en', flag: 'gb', label: 'English', title: 'Sacked: Your Last Day', file: 'Setup.exe',
        url: `${IA}sacked_your_last_day_pc/Setup.exe`, size: 75160974,
        sha256: 'e77cb450636de1f6d6031856a2e67de5e8adb4178ec9e2bf3c59d6f506070f89' },
      { code: 'hu', label: 'Magyar', title: 'Ki vagy rúgva!', file: 'SACKED.ISO',
        url: `${IA}ki-vagy-rugva/SACKED.ISO`, size: 75585536,
        sha256: '685a8f35d77fa67e368a54fa81d1e065b361f00cc38fe2b46c3bb060b948bbaf' },
      { code: 'pl', label: 'Polski', title: 'Zemsta Urzędasa', file: 'ZEMSTA URZĘDASA (1).exe',
        url: `${IA}zemsta-urze-dasa-1/ZEMSTA%20URZ%C4%98DASA%20%281%29.exe`, size: 75325442,
        sha256: 'c44c49a86c8d876cb92ebea447379aefd4aa6d0618eec4f961553d7e14ac0f57' },
      { code: 'nl', label: 'Nederlands', title: 'Ontslagen', file: 'cd.zip',
        url: `${IA}ontslagenneemwraakopjelaatstewerkdag/cd.zip`, size: 59020475,
        sha256: '5587efe35f966aa4a55a5cc993bd8c7b811f76b751a586e6d594c8ca47ea7d11' },
      { code: 'ru', label: 'Русский', title: 'Оторва!', file: 'Sacked.img',
        url: `${IA}sacked_202601/Sacked.img`, size: 138601008,
        sha256: 'bf9c851d4c1a02acbb62019a680400cb70579fbb5e87795e3df3d1031dd8fa00' },
    ],
  },
  crazyschool: {
    id: 'crazyschool',
    name: 'Crazy School',
    full: 'Crazy School – Schulverweis!',
    stem: 'CrazySchool',
    size: 'about 240 MiB',
    web: 'crazyschool',
    icon: 'crazyschool.svg',
    languages: [
      { code: 'de', label: 'Deutsch', title: 'Crazy School – Schulverweis!', file: 'CrazySchool.iso',
        url: `${IA}crazyschoolgame/CrazySchool.iso`, size: 122435584,
        sha256: '702652d03bbd3feaad521bc8ccfbce6f5a68741bc9d4840a73462db80a830a16' },
      { code: 'ru', label: 'Русский', title: 'Оторва в школе', file: 'Crazy School.img',
        url: `${IA}2_20240726_20240726_0734/Crazy%20School.img`, size: 98442960,
        sha256: '1624b02ce7ec7c7244de69ed54a80d529c142d5b95592b3d5c293a1bec2ea79d' },
      { code: 'pl', label: 'Polski', title: 'Zemsta kujona', file: 'setup.exe',
        url: `${IA}setup_20200415/setup.exe`, size: 61869354,
        sha256: 'e6a54cc469b8ce7eb86cdb357b25e169862d46cd6f502ebe9e352768d93aea8a' },
    ],
  },
};

/* The game's icon on the page: the tab's and every img.game-icon. */
export function showIcon(game) {
  for (const l of document.querySelectorAll('link[rel=icon]')) l.href = game.icon;
  for (const i of document.querySelectorAll('img.game-icon')) i.src = game.icon;
}

/* The game a page is for: ?game=<id> (or #<id>), else the default. */
export function gameOf(location) {
  const p = new URLSearchParams(location.search).get('game') || location.hash.replace(/^#/, '');
  return GAMES[p] || GAMES.sacked;
}
