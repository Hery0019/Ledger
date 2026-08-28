#pragma once

#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "storage/engine.h"

namespace ledger {

// Moteur fichiers texte : un dossier par table sous le dossier de la base.
//
//   <base>/LOCK               présent tant qu'un processus a la base ouverte
//   <base>/<table>/schema.txt
//   <base>/<table>/rows.txt   append-only, voir storage/codec.h
//
// Modèle : au premier accès, rows.txt est rejoué intégralement en mémoire
// (map rowid -> Row) ; ensuite chaque écriture est ajoutée au fichier (flush,
// pas de fsync : on se protège d'un crash du programme, pas d'une coupure de
// courant) et appliquée en mémoire. Quand les tombstones dépassent un seuil,
// le fichier est réécrit sans eux (compaction, via fichier temporaire + rename).
//
// Une dernière ligne tronquée (crash pendant un append) est ignorée et
// signalée dans warnings() ; toute autre anomalie est une Corruption.
//
// Thread-safe (un mutex pour toute la base) ; mono-processus (fichier LOCK).
class FileEngine final : public IStorageEngine {
public:
    // Crée le dossier s'il n'existe pas, prend le verrou. IoError si la base
    // est déjà ouverte par un autre processus.
    static Result<std::unique_ptr<FileEngine>> open(const std::filesystem::path& dir);
    ~FileEngine() override;

    FileEngine(const FileEngine&) = delete;
    FileEngine& operator=(const FileEngine&) = delete;

    Result<void> createTable(const TableSchema& schema) override;
    Result<void> dropTable(std::string_view table) override;
    Result<RowId> insert(std::string_view table, const Row& row) override;
    Result<void> scan(std::string_view table,
                      const std::function<bool(RowId, const Row&)>& visit) override;
    Result<void> update(std::string_view table, RowId id, const Row& row) override;
    Result<void> remove(std::string_view table, RowId id) override;
    Result<void> compact(std::string_view table) override;
    Result<std::vector<TableSchema>> loadSchemas() override;

    // Avertissements accumulés (lignes tronquées ignorées...). Vidés à l'appel.
    std::vector<std::string> takeWarnings();

    [[nodiscard]] const std::filesystem::path& directory() const noexcept { return dir_; }

    // Seuil de compaction automatique : tombstones > kCompactMinTombstones
    // ET tombstones > lignes vivantes.
    static constexpr std::size_t kCompactMinTombstones = 1000;

private:
    explicit FileEngine(std::filesystem::path dir) : dir_(std::move(dir)) {}

    struct Table {
        TableSchema schema;
        std::map<RowId, Row> rows;
        RowId nextId = 1;
        std::size_t tombstones = 0;
        bool loaded = false;
        std::FILE* out = nullptr;  // rows.txt ouvert en append, nullptr tant que non chargé
    };

    [[nodiscard]] std::filesystem::path tableDir(std::string_view table) const;
    Result<Table*> loaded(std::string_view table);  // charge rows.txt si besoin
    Result<void> loadRows(Table& t);
    Result<void> appendLine(Table& t, const std::string& line);
    Result<void> rewrite(Table& t);  // compaction effective
    Result<void> maybeCompact(Table& t);
    static void closeFile(Table& t) noexcept;

    std::filesystem::path dir_;
    std::map<std::string, Table, std::less<>> tables_;  // toutes les tables connues (schéma chargé)
    std::vector<std::string> warnings_;
    std::mutex mu_;
};

}  // namespace ledger
