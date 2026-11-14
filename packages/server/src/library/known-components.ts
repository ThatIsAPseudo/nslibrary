import type { AppComponent, ContentMetaType } from "@nslib/shared";
import {
  applicationIdForPatch,
  guessApplicationIdForAddon,
  inferTypeFromTitleId,
} from "@nslib/shared";
import type { Db } from "../db/client";
import { titledbTitles, titledbVersions } from "../db/schema";

/** The part of a scanned component needed to compare it with the metadata inventory. */
export interface LocalComponent {
  contentMetaId: number;
  titleId: string;
  type: ContentMetaType;
  version: number | null;
  name: string;
  size: number;
  releaseDate: number | null;
}

interface ExpectedComponent {
  titleId: string;
  type: ContentMetaType;
  version: number | null;
  name: string;
  size: number | null;
  releaseDate: number | null;
}

export interface KnownComponentIndex {
  titlesById: Map<string, typeof titledbTitles.$inferSelect>;
  dlcByApplication: Map<string, (typeof titledbTitles.$inferSelect)[]>;
  versionsByApplication: Map<string, (typeof titledbVersions.$inferSelect)[]>;
}

const TYPE_ORDER: Record<ContentMetaType, number> = { application: 0, patch: 1, addon: 2 };

/** Reads the optional metadata inventory once for a detail request or an entire device catalog. */
export function createKnownComponentIndex(db: Db, titledbEnabled: boolean): KnownComponentIndex {
  const titlesById = new Map<string, typeof titledbTitles.$inferSelect>();
  const dlcByApplication = new Map<string, (typeof titledbTitles.$inferSelect)[]>();
  const versionsByApplication = new Map<string, (typeof titledbVersions.$inferSelect)[]>();
  if (!titledbEnabled) return { titlesById, dlcByApplication, versionsByApplication };

  const titles = db.select().from(titledbTitles).all();
  for (const title of titles) {
    titlesById.set(title.titleId.toUpperCase(), title);
    if (inferTypeFromTitleId(title.titleId) !== "addon") continue;
    // Older title databases omit baseId/applicationId; infer the base from an add-on ID as the
    // library's existing DLC association fallback does.
    const applicationId = (
      title.applicationId ?? guessApplicationIdForAddon(title.titleId)
    ).toUpperCase();
    const group = dlcByApplication.get(applicationId);
    if (group) group.push(title);
    else dlcByApplication.set(applicationId, [title]);
  }
  for (const version of db.select().from(titledbVersions).all()) {
    const group = versionsByApplication.get(version.titleId.toUpperCase());
    if (group) group.push(version);
    else versionsByApplication.set(version.titleId.toUpperCase(), [version]);
  }
  return { titlesById, dlcByApplication, versionsByApplication };
}

/**
 * Builds the component inventory from titledb and overlays scanned files. The files are never
 * invented: expected components without a corresponding file have `inLibrary: false` and a null
 * `contentMetaId`, so clients can describe them but cannot accidentally offer them for install.
 */
export function listKnownComponents(
  index: KnownComponentIndex,
  applicationId: string,
  local: LocalComponent[],
): AppComponent[] {
  const id = applicationId.toUpperCase();
  const expected: ExpectedComponent[] = [];
  const base = index.titlesById.get(id);

  // A patch or DLC cannot be played without its base game. Show the missing base even if the
  // title database has no row for it; when available, titledb supplies its name, date, and size.
  if (base || local.some((item) => item.type !== "application")) {
    expected.push({
      titleId: id,
      type: "application",
      version: 0,
      name: base?.name ?? local.find((item) => item.type === "application")?.name ?? id,
      size: base?.size ?? null,
      releaseDate: base?.releaseDate ?? null,
    });
  }

  const versions = index.versionsByApplication.get(id) ?? [];
  const knownVersions = new Set<number>();
  for (const item of versions) {
    // Version zero is not an update. Some source files use it to date the base game itself.
    if (item.version <= 0) continue;
    knownVersions.add(item.version);
    expected.push({
      titleId: applicationIdForPatch(id),
      type: "patch",
      version: item.version,
      name: base?.name ? `${base.name} Update` : "Update",
      size: item.size,
      releaseDate: item.releaseDate,
    });
  }
  // Some titles list a latest version but no versions.json entry (and older imports may have
  // only the base entry). Keep that version visible, with date/size unknown.
  if (
    base?.latestVersion != null &&
    base.latestVersion > 0 &&
    !knownVersions.has(base.latestVersion)
  ) {
    expected.push({
      titleId: applicationIdForPatch(id),
      type: "patch",
      version: base.latestVersion,
      name: base.name ? `${base.name} Update` : "Update",
      size: null,
      releaseDate: null,
    });
  }

  for (const item of index.dlcByApplication.get(id) ?? []) {
    expected.push({
      titleId: item.titleId,
      type: "addon",
      version: item.latestVersion ?? 0,
      name: item.name ?? item.titleId,
      size: item.size,
      releaseDate: item.releaseDate,
    });
  }

  const used = new Set<number>();
  const result: AppComponent[] = [];
  for (const item of expected) {
    // Base identity is its title ID; DLC identity is its title ID and listed version; updates
    // are the game's patch title ID plus the exact version in versions.json.
    const match = local.find((candidate) => {
      if (used.has(candidate.contentMetaId) || candidate.type !== item.type) return false;
      if (candidate.titleId.toUpperCase() !== item.titleId.toUpperCase()) return false;
      return item.type === "application" || candidate.version === item.version;
    });
    if (match) used.add(match.contentMetaId);
    result.push({
      ...item,
      ...(match && { version: match.version }),
      ...(match?.name && item.type === "addon" && item.name === item.titleId
        ? { name: match.name }
        : {}),
      size: match?.size ?? item.size,
      releaseDate: match?.releaseDate ?? item.releaseDate,
      inLibrary: match !== undefined,
      contentMetaId: match?.contentMetaId ?? null,
    });
  }

  for (const item of local) {
    if (used.has(item.contentMetaId)) continue;
    result.push({
      titleId: item.titleId,
      type: item.type,
      version: item.version,
      name: item.name,
      size: item.size,
      releaseDate: item.releaseDate,
      inLibrary: true,
      contentMetaId: item.contentMetaId,
    });
  }

  return result.sort(
    (a, b) =>
      TYPE_ORDER[a.type] - TYPE_ORDER[b.type] ||
      (b.version ?? -1) - (a.version ?? -1) ||
      a.name.localeCompare(b.name) ||
      a.titleId.localeCompare(b.titleId),
  );
}
