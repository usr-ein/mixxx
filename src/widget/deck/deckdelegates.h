#pragma once

#include <QCache>
#include <QPixmap>
#include <QStyledItemDelegate>

class BaseSqlTableModel;

namespace mixxx {
namespace deck {

/// Paints every menu row: sources, categories, values, playlists, folders.
///
/// One delegate rather than five, because they differ only in which of the four
/// slots they fill — mark, title, detail, cover.
class MenuRowDelegate : public QStyledItemDelegate {
    Q_OBJECT

  public:
    explicit MenuRowDelegate(QObject* pParent = nullptr);

    void setRowHeight(int height) {
        m_rowHeight = height;
    }

    void paint(QPainter* pPainter,
            const QStyleOptionViewItem& option,
            const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option,
            const QModelIndex& index) const override;

    /// Forget every decoded cover.
    ///
    /// Needed when a cover *arrives*: a row drawn before it landed cached the
    /// grey placeholder, and without this the square would stay grey for as
    /// long as the delegate lived.
    void clearCoverCache() {
        m_coverCache.clear();
    }

  private:
    /// Covers are loaded off disk on demand and kept, because a list redraws on
    /// every encoder detent and decoding a JPEG per row per detent is visible.
    QPixmap coverFor(const QStringList& paths, int size) const;

    int m_rowHeight = 80;
    mutable QCache<QString, QPixmap> m_coverCache;
};

/// Paints a track row: cover, title, artist, BPM, key (browser-prd.md 8.1).
///
/// Reads the sibling columns off the model rather than being handed a struct,
/// which is what lets it sit on top of BaseSqlTableModel unchanged.
class TrackRowDelegate : public QStyledItemDelegate {
    Q_OBJECT

  public:
    explicit TrackRowDelegate(QObject* pParent = nullptr);

    void setRowHeight(int height) {
        m_rowHeight = height;
    }
    /// The deck's current key, for harmonic colouring. An invalid id turns the
    /// colouring off, which is what "nothing is loaded" should look like.
    void setPlayingKeyId(int keyId) {
        m_playingKeyId = keyId;
    }
    /// deck_library id of the track on the deck, so the row it came from can be
    /// marked. -1 when nothing is loaded.
    void setLoadedTrackId(int trackId) {
        m_loadedTrackId = trackId;
    }
    /// In the info layout the row is one line: title left, and the value of
    /// whatever the list is sorted by right (browser-prd.md 8.2).
    void setInfoLayout(bool infoLayout) {
        m_infoLayout = infoLayout;
    }
    /// Which column the right-hand value comes from in the info layout.
    void setSecondaryColumn(int column) {
        m_secondaryColumn = column;
    }

    /// Column indices, resolved once when the model changes.
    ///
    /// Not looked up per paint: this delegate runs for every visible row on
    /// every encoder detent, and resolving a name to an index in there turns a
    /// scroll into a few hundred string comparisons a frame.
    struct Columns {
        int cover = -1;
        int title = -1;
        int artist = -1;
        int bpm = -1;
        int key = -1;
        int keyId = -1;
        int color = -1;
        /// The view's id column, for spotting the loaded track.
        int trackId = -1;
    };
    void setColumns(const Columns& columns) {
        m_columns = columns;
    }

    void paint(QPainter* pPainter,
            const QStyleOptionViewItem& option,
            const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option,
            const QModelIndex& index) const override;

    /// Forget every decoded cover.
    ///
    /// Needed when a cover *arrives*: a row drawn before it landed cached the
    /// grey placeholder, and without this the square would stay grey for as
    /// long as the delegate lived.
    void clearCoverCache() {
        m_coverCache.clear();
    }

  private:
    QPixmap coverFor(const QString& path, int size) const;

    int m_rowHeight = 72;
    int m_playingKeyId = 0;
    int m_loadedTrackId = -1;
    bool m_infoLayout = false;
    int m_secondaryColumn = -1;
    Columns m_columns;
    mutable QCache<QString, QPixmap> m_coverCache;
};

} // namespace deck
} // namespace mixxx
