-- Optional package sizes from titledb entries and version metadata.
ALTER TABLE `titledb_titles` ADD `size` integer;
--> statement-breakpoint
ALTER TABLE `titledb_versions` ADD `size` integer;
