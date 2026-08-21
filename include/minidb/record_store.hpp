#pragma once

#include "minidb/pager.hpp"
#include "minidb/record_id.hpp"
#include "minidb/record_page.hpp"
#include "minidb/row.hpp"

#include <utility>
#include <vector>

namespace minidb {

class RecordStore {
public:
    using ScanEntry = std::pair<RecordId, Row>;

    explicit RecordStore(Pager& pager);
    RecordStore(Pager& pager, PageId headPageId);

    [[nodiscard]] PageId headPageId() const noexcept { return headPageId_; }
    [[nodiscard]] bool empty() const noexcept { return headPageId_ == INVALID_PAGE_ID; }

    [[nodiscard]] RecordId insert(const Row& row);
    [[nodiscard]] Row get(RecordId recordId);
    void update(RecordId recordId, const Row& row);
    void erase(RecordId recordId);
    [[nodiscard]] std::vector<ScanEntry> scan();

private:
    Pager& pager_;
    PageId headPageId_;

    [[nodiscard]] PageId allocateRecordPage();
    [[nodiscard]] RecordPage findRecordPage(RecordId recordId);
    void validateHeadPageId() const;
    void validateChain();
    void validateRecordId(RecordId recordId) const;
};

} // namespace minidb
