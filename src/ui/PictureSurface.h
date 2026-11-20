#pragma once

#include <QObject>
#include <QRectF>
#include <QSizeF>
#include <QString>

namespace rl {

class FisheyeAims;

// The picture rules of one video pane, once (architecture review, picture surface): how far it is
// zoomed and panned and where that stops, when a press has moved far enough to pick the pane
// up, what shape its picture is, and whether the picture is a fisheye circle drawn through its
// view. Live View, Playback, the pop-out window and the Events clip player each make one
// (QML type PictureSurface) and bind its inputs to their video item; none of them holds the
// arithmetic. The shape maths is FrameShape's (frameshape::), the aim store FisheyeAims'.
//
// The dewarp rule: a picture is drawn through the fisheye view only when the camera is a
// fisheye, a view other than the raw circle is chosen, the picture is not Frigate's flat copy,
// and the picture IS a circle (square within 1 %; frameshape::isCircle). While no picture has
// been seen (pictureRatio 0) the camera's lens flag and view decide, so a pane does not flash
// flat before its first frame.
class PictureSurface : public QObject
{
    Q_OBJECT

    // ---- Inputs: bound by the pane to its video item -------------------------------------
    Q_PROPERTY(QSizeF viewport READ viewport WRITE setViewport NOTIFY viewportChanged)
    Q_PROPERTY(QRectF contentRect READ contentRect WRITE setContentRect NOTIFY contentRectChanged)
    // The decoded picture's shape (width / height, before any rotation) from the video's
    // source size; 0 until a frame has arrived. Valid while dewarping too.
    Q_PROPERTY(double pictureRatio READ pictureRatio WRITE setPictureRatio NOTIFY pictureRatioChanged)
    Q_PROPERTY(int rotation READ rotation WRITE setRotation NOTIFY rotationChanged)
    // A picture is on screen, so its drawn shape may be measured.
    Q_PROPERTY(bool streaming READ streaming WRITE setStreaming NOTIFY streamingChanged)
    Q_PROPERTY(bool fisheyeLens READ fisheyeLens WRITE setFisheyeLens NOTIFY fisheyeLensChanged)
    Q_PROPERTY(QString fisheyeMode READ fisheyeMode WRITE setFisheyeMode NOTIFY fisheyeModeChanged)
    // Frigate's flattened copy of a fisheye: never dewarped, never a circle.
    Q_PROPERTY(bool flatCopy READ flatCopy WRITE setFlatCopy NOTIFY flatCopyChanged)
    Q_PROPERTY(QString cameraKey READ cameraKey WRITE setCameraKey NOTIFY cameraKeyChanged)
    Q_PROPERTY(rl::FisheyeAims *aims READ aims WRITE setAims NOTIFY aimsChanged)

    // ---- State and answers ---------------------------------------------------------------
    // Digital zoom 1..8 and the pan that keeps the picture over its viewport. Writable so a
    // pane bar button can set them; a dewarp resets them (a dewarped view zooms itself).
    Q_PROPERTY(double zoom READ zoom WRITE setZoom NOTIFY zoomChanged)
    Q_PROPERTY(double panX READ panX WRITE setPanX NOTIFY panChanged)
    Q_PROPERTY(double panY READ panY WRITE setPanY NOTIFY panChanged)
    // Zoomed in: a drag pans the image rather than picking the pane up.
    Q_PROPERTY(bool panning READ panning NOTIFY zoomChanged)
    // The measured shape of the drawn picture (rotation included); 0 until one was seen.
    Q_PROPERTY(double ratio READ ratio NOTIFY ratioChanged)
    Q_PROPERTY(bool dewarp READ dewarp NOTIFY dewarpChanged)
    Q_PROPERTY(bool circle READ circle NOTIFY dewarpChanged)
    // The shape the picture is drawn at: its view's for a circle, else its own.
    Q_PROPERTY(double shownRatio READ shownRatio NOTIFY dewarpChanged)
    Q_PROPERTY(double dragThreshold READ dragThreshold CONSTANT)

public:
    explicit PictureSurface(QObject *parent = nullptr);

    QSizeF viewport() const { return m_viewport; }
    void setViewport(const QSizeF &size);
    QRectF contentRect() const { return m_content; }
    void setContentRect(const QRectF &rect);
    double pictureRatio() const { return m_pictureRatio; }
    void setPictureRatio(double ratio);
    int rotation() const { return m_rotation; }
    void setRotation(int degrees);
    bool streaming() const { return m_streaming; }
    void setStreaming(bool on);
    bool fisheyeLens() const { return m_lens; }
    void setFisheyeLens(bool on);
    QString fisheyeMode() const { return m_mode; }
    void setFisheyeMode(const QString &mode);
    bool flatCopy() const { return m_flat; }
    void setFlatCopy(bool on);
    QString cameraKey() const { return m_key; }
    void setCameraKey(const QString &key);
    FisheyeAims *aims() const { return m_aims; }
    void setAims(FisheyeAims *aims);

    double zoom() const { return m_zoom; }
    void setZoom(double zoom);
    double panX() const { return m_panX; }
    void setPanX(double x);
    double panY() const { return m_panY; }
    void setPanY(double y);
    bool panning() const { return m_zoom > 1.0; }
    double ratio() const { return m_ratio; }
    bool dewarp() const { return m_dewarp; }
    bool circle() const;
    double shownRatio() const;
    double dragThreshold() const;

    // One wheel notch: 15 % in or out, 1..8; back at 1 the pan is dropped, else kept in bounds.
    Q_INVOKABLE void zoomStep(bool zoomIn);
    // Pan by a drag step, kept in bounds.
    Q_INVOKABLE void panBy(double dx, double dy);
    // The pane bar's zoom button: back to 1 when zoomed in, else 2x.
    Q_INVOKABLE void toggleZoom();
    Q_INVOKABLE void resetZoom();
    // A press that has moved (dx, dy) from where it started has moved far enough to be a drag.
    Q_INVOKABLE bool pastDragThreshold(double dx, double dy) const;
    // The camera's dewarped view has been turned from where it starts.
    Q_INVOKABLE bool aimed() const;
    // Double-click on a turned view puts it back and says so (true); false: nothing to put back.
    Q_INVOKABLE bool resetAim();

signals:
    void viewportChanged();
    void contentRectChanged();
    void pictureRatioChanged();
    void rotationChanged();
    void streamingChanged();
    void fisheyeLensChanged();
    void fisheyeModeChanged();
    void flatCopyChanged();
    void cameraKeyChanged();
    void aimsChanged();
    void zoomChanged();
    void panChanged();
    void ratioChanged();
    void dewarpChanged();

private:
    void recompute();
    void clampPan();

    QSizeF m_viewport;
    QRectF m_content;
    double m_pictureRatio = 0;
    int m_rotation = 0;
    bool m_streaming = false;
    bool m_lens = false;
    QString m_mode = QStringLiteral("fisheye");
    bool m_flat = false;
    QString m_key;
    FisheyeAims *m_aims = nullptr;
    double m_zoom = 1.0;
    double m_panX = 0;
    double m_panY = 0;
    double m_ratio = 0;
    bool m_dewarp = false;
};

} // namespace rl
