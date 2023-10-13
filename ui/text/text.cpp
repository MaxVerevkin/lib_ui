// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "ui/text/text.h"

#include "ui/effects/spoiler_mess.h"
#include "ui/text/text_extended_data.h"
#include "ui/text/text_isolated_emoji.h"
#include "ui/text/text_parser.h"
#include "ui/text/text_renderer.h"
#include "ui/basic_click_handlers.h"
#include "ui/painter.h"
#include "base/platform/base_platform_info.h"
#include "styles/style_basic.h"

namespace Ui {

const QString kQEllipsis = u"..."_q;

} // namespace Ui

namespace Ui::Text {
namespace {

constexpr auto kDefaultSpoilerCacheCapacity = 24;

[[nodiscard]] Qt::LayoutDirection StringDirection(
		const QString &str,
		int from,
		int to) {
	auto p = reinterpret_cast<const ushort*>(str.unicode()) + from;
	const auto end = p + (to - from);
	while (p < end) {
		uint ucs4 = *p;
		if (QChar::isHighSurrogate(ucs4) && p < end - 1) {
			ushort low = p[1];
			if (QChar::isLowSurrogate(low)) {
				ucs4 = QChar::surrogateToUcs4(ucs4, low);
				++p;
			}
		}
		switch (QChar::direction(ucs4)) {
		case QChar::DirL:
			return Qt::LeftToRight;
		case QChar::DirR:
		case QChar::DirAL:
			return Qt::RightToLeft;
		default:
			break;
		}
		++p;
	}
	return Qt::LayoutDirectionAuto;
}

bool IsParagraphSeparator(QChar ch) {
	switch (ch.unicode()) {
	case QChar::LineFeed:
		return true;
	default:
		break;
	}
	return false;
}

} // namespace
} // namespace Ui::Text

const TextParseOptions kDefaultTextOptions = {
	TextParseLinks | TextParseMultiline, // flags
	0, // maxw
	0, // maxh
	Qt::LayoutDirectionAuto, // dir
};

const TextParseOptions kMarkupTextOptions = {
	TextParseLinks | TextParseMultiline | TextParseMarkdown, // flags
	0, // maxw
	0, // maxh
	Qt::LayoutDirectionAuto, // dir
};

const TextParseOptions kPlainTextOptions = {
	TextParseMultiline, // flags
	0, // maxw
	0, // maxh
	Qt::LayoutDirectionAuto, // dir
};

namespace Ui::Text {

struct SpoilerMessCache::Entry {
	SpoilerMessCached mess;
	QColor color;
};

SpoilerMessCache::SpoilerMessCache(int capacity) : _capacity(capacity) {
	Expects(capacity > 0);

	_cache.reserve(capacity);
}

SpoilerMessCache::~SpoilerMessCache() = default;

not_null<SpoilerMessCached*> SpoilerMessCache::lookup(QColor color) {
	for (auto &entry : _cache) {
		if (entry.color == color) {
			return &entry.mess;
		}
	}
	Assert(_cache.size() < _capacity);
	_cache.push_back({
		.mess = Ui::SpoilerMessCached(DefaultTextSpoilerMask(), color),
		.color = color,
	});
	return &_cache.back().mess;
}

void SpoilerMessCache::reset() {
	_cache.clear();
}

not_null<SpoilerMessCache*> DefaultSpoilerCache() {
	struct Data {
		Data() : cache(kDefaultSpoilerCacheCapacity) {
			style::PaletteChanged() | rpl::start_with_next([=] {
				cache.reset();
			}, lifetime);
		}

		SpoilerMessCache cache;
		rpl::lifetime lifetime;
	};

	static auto data = Data();
	return &data.cache;
}

GeometryDescriptor SimpleGeometry(
		int availableWidth,
		int fontHeight,
		int elisionHeight,
		int elisionRemoveFromEnd,
		bool elisionOneLine,
		bool elisionBreakEverywhere) {
	constexpr auto wrap = [](
			Fn<LineGeometry(LineGeometry)> layout,
			bool breakEverywhere = false) {
		return GeometryDescriptor{ std::move(layout), breakEverywhere };
	};

	// Try to minimize captured values (to minimize Fn allocations).
	if (!elisionOneLine && !elisionHeight) {
		return wrap([=](LineGeometry line) {
			line.width = availableWidth;
			return line;
		});
	} else if (elisionOneLine) {
		return wrap([=](LineGeometry line) {
			line.elided = true;
			line.width = availableWidth - elisionRemoveFromEnd;
			return line;
		}, elisionBreakEverywhere);
	} else if (!elisionRemoveFromEnd) {
		return wrap([=](LineGeometry line) {
			if (line.top + fontHeight * 2 > elisionHeight) {
				line.elided = true;
			}
			line.width = availableWidth;
			return line;
		});
	} else {
		return wrap([=](LineGeometry line) {
			if (line.top + fontHeight * 2 > elisionHeight) {
				line.elided = true;
				line.width = availableWidth - elisionRemoveFromEnd;
			} else {
				line.width = availableWidth;
			}
			return line;
		});
	}
};

void ValidateBlockPaintCache(
		BlockPaintCache &cache,
		const style::ParagraphStyle &st) {
	const auto icon = st.icon.empty() ? nullptr : &st.icon;
	if (!cache.corners.isNull()
		&& cache.bgCached == cache.bg
		&& cache.outlineCached == cache.outline
		&& (!st.header || cache.headerCached == cache.header)
		&& (!icon || cache.iconCached == cache.icon)) {
		return;
	}
	cache.bgCached = cache.bg;
	cache.outlineCached = cache.outline;
	if (st.header) {
		cache.headerCached = cache.header;
	}
	if (!st.icon.empty()) {
		cache.iconCached = cache.icon;
	}
	const auto radius = st.radius;
	const auto header = st.header;
	const auto outline = st.outline;
	const auto iconsize = icon
		? std::max(
			icon->width() + st.iconPosition.x(),
			icon->height() + st.iconPosition.y())
		: 0;
	const auto corner = std::max({ header, radius, outline, iconsize });
	const auto middle = st::lineWidth;
	const auto side = 2 * corner + middle;
	const auto full = QSize(side, side);
	const auto ratio = style::DevicePixelRatio();
	auto image = QImage(full * ratio, QImage::Format_ARGB32_Premultiplied);
	image.fill(Qt::transparent);
	image.setDevicePixelRatio(ratio);
	auto p = QPainter(&image);
	auto hq = PainterHighQualityEnabler(p);
	p.setPen(Qt::NoPen);

	auto rect = QRect(QPoint(), full);
	if (header) {
		p.setBrush(cache.header);
		p.setClipRect(outline, 0, side - outline, header);
		p.drawRoundedRect(0, 0, side, corner + radius, radius, radius);
	}
	if (outline) {
		p.setBrush(cache.outline);
		p.setClipRect(0, 0, outline, side);
		p.drawRoundedRect(0, 0, outline + radius * 2, side, radius, radius);
	}
	p.setBrush(cache.bg);
	p.setClipRect(outline, header, side - outline, side - header);
	p.drawRoundedRect(0, 0, side, side, radius, radius);
	if (icon) {
		const auto left = side - icon->width() - st.iconPosition.x();
		const auto top = st.iconPosition.y();
		icon->paint(p, left, top, side, cache.icon);
	}

	p.end();
	cache.corners = std::move(image);
}

void FillBlockPaint(
		QPainter &p,
		QRect rect,
		const BlockPaintCache &cache,
		const style::ParagraphStyle &st,
		SkipBlockPaintParts parts) {
	const auto &image = cache.corners;
	const auto ratio = int(image.devicePixelRatio());
	const auto iwidth = image.width() / ratio;
	const auto iheight = image.height() / ratio;
	const auto imiddle = st::lineWidth;
	const auto ihalf = (iheight - imiddle) / 2;
	const auto x = rect.left();
	const auto width = rect.width();
	auto y = rect.top();
	auto height = rect.height();
	if (!parts.skipTop) {
		const auto top = std::min(height, ihalf);
		p.drawImage(
			QRect(x, y, ihalf, top),
			image,
			QRect(0, 0, ihalf * ratio, top * ratio));
		p.drawImage(
			QRect(x + width - ihalf, y, ihalf, top),
			image,
			QRect((iwidth - ihalf) * ratio, 0, ihalf * ratio, top * ratio));
		if (const auto middle = width - 2 * ihalf) {
			const auto header = st.header;
			const auto fillHeader = std::min(header, top);
			if (fillHeader) {
				p.fillRect(x + ihalf, y, middle, fillHeader, cache.header);
			}
			if (const auto fillBody = top - fillHeader) {
				p.fillRect(
					QRect(x + ihalf, y + fillHeader, middle, fillBody),
					cache.bg);
			}
		}
		height -= top;
		if (!height) {
			return;
		}
		y += top;
		rect.setTop(y);
	}
	if (!parts.skipBottom) {
		const auto bottom = std::min(height, ihalf);
		p.drawImage(
			QRect(x, y + height - bottom, ihalf, bottom),
			image,
			QRect(
				0,
				(iheight - bottom) * ratio,
				ihalf * ratio,
				bottom * ratio));
		p.drawImage(
			QRect(
				x + width - ihalf,
				y + height - bottom,
				ihalf,
				bottom),
			image,
			QRect(
				(iwidth - ihalf) * ratio,
				(iheight - bottom) * ratio,
				ihalf * ratio,
				bottom * ratio));
		if (const auto middle = width - 2 * ihalf) {
			p.fillRect(
				QRect(x + ihalf, y + height - bottom, middle, bottom),
				cache.bg);
		}
		height -= bottom;
		if (!height) {
			return;
		}
		rect.setHeight(height);
	}
	const auto outline = st.outline;
	if (outline) {
		p.fillRect(x, y, outline, height, cache.outline);
	}
	p.fillRect(x + outline, y, width - outline, height, cache.bg);
}

String::ExtendedWrap::ExtendedWrap() noexcept = default;

String::ExtendedWrap::ExtendedWrap(ExtendedWrap &&other) noexcept
: unique_ptr(std::move(other)) {
	adjustFrom(&other);
}

String::ExtendedWrap &String::ExtendedWrap::operator=(
		ExtendedWrap &&other) noexcept {
	*static_cast<unique_ptr*>(this) = std::move(other);
	adjustFrom(&other);
	return *this;
}

String::ExtendedWrap::ExtendedWrap(
	std::unique_ptr<ExtendedData> &&other) noexcept
: unique_ptr(std::move(other)) {
	Assert(!get() || !get()->spoiler);
}

String::ExtendedWrap &String::ExtendedWrap::operator=(
		std::unique_ptr<ExtendedData> &&other) noexcept {
	*static_cast<unique_ptr*>(this) = std::move(other);
	Assert(!get() || !get()->spoiler);
	return *this;
}

String::ExtendedWrap::~ExtendedWrap() = default;

void String::ExtendedWrap::adjustFrom(const ExtendedWrap *other) {
	const auto data = get();
	if (data && data->spoiler) {
		const auto raw = [](auto pointer) {
			return reinterpret_cast<quintptr>(pointer);
		};
		const auto otherText = raw(data->spoiler->link->text().get());
		data->spoiler->link->setText(
			reinterpret_cast<String*>(otherText + raw(this) - raw(other)));
	}
}

String::String(int32 minResizeWidth)
: _minResizeWidth(minResizeWidth) {
}

String::String(
	const style::TextStyle &st,
	const QString &text,
	const TextParseOptions &options,
	int32 minResizeWidth)
: _minResizeWidth(minResizeWidth) {
	setText(st, text, options);
}

String::String(
	const style::TextStyle &st,
	const TextWithEntities &textWithEntities,
	const TextParseOptions &options,
	int32 minResizeWidth,
	const std::any &context)
: _minResizeWidth(minResizeWidth) {
	setMarkedText(st, textWithEntities, options, context);
}


String::String(String &&other) = default;

String &String::operator=(String &&other) = default;

String::~String() = default;

void String::setText(const style::TextStyle &st, const QString &text, const TextParseOptions &options) {
	_st = &st;
	clear();
	{
		Parser parser(this, { text }, options, {});
	}
	recountNaturalSize(true, options.dir);
}

void String::recountNaturalSize(
		bool initial,
		Qt::LayoutDirection optionsDirection) {
	auto lastNewline = (NewlineBlock*)nullptr;
	auto lastNewlineStart = 0;
	const auto computeParagraphDirection = [&](int paragraphEnd) {
		const auto direction = (optionsDirection != Qt::LayoutDirectionAuto)
			? optionsDirection
			: StringDirection(_text, lastNewlineStart, paragraphEnd);
		if (lastNewline) {
			lastNewline->_paragraphLTR = (direction == Qt::LeftToRight);
			lastNewline->_paragraphRTL = (direction == Qt::RightToLeft);
		} else {
			_startParagraphLTR = (direction == Qt::LeftToRight);
			_startParagraphRTL = (direction == Qt::RightToLeft);
		}
	};

	auto pindex = paragraphIndex(nullptr);
	auto paragraph = paragraphByIndex(pindex);
	auto ppadding = paragraphPadding(paragraph);

	_maxWidth = 0;
	_minHeight = ppadding.top();
	auto lineHeight = 0;
	auto maxWidth = QFixed();
	auto width = QFixed(ppadding.left() + ppadding.right());
	auto last_rBearing = QFixed();
	auto last_rPadding = QFixed();
	for (auto &block : _blocks) {
		const auto b = block.get();
		const auto _btype = b->type();
		const auto blockHeight = CountBlockHeight(b, _st);
		if (_btype == TextBlockType::Newline) {
			if (!lineHeight) {
				lineHeight = blockHeight;
			}
			const auto index = paragraphIndex(b);
			if (pindex != index) {
				_minHeight += ppadding.bottom();
				pindex = index;
				paragraph = paragraphByIndex(pindex);
				ppadding = paragraphPadding(paragraph);
				_minHeight += ppadding.top();
				ppadding.setTop(0);
			}
			if (initial) {
				computeParagraphDirection(b->position());
			}
			lastNewlineStart = b->position();
			lastNewline = &block.unsafe<NewlineBlock>();

			_minHeight += lineHeight;
			lineHeight = 0;
			last_rBearing = 0;// b->f_rbearing(); (0 for newline)
			last_rPadding = 0;// b->f_rpadding(); (0 for newline)

			accumulate_max(maxWidth, width);
			width = ppadding.left() + ppadding.right();
			// + (b->f_width() - last_rBearing); (0 for newline)
			continue;
		}

		auto b__f_rbearing = b->f_rbearing(); // cache

		// We need to accumulate max width after each block, because
		// some blocks have width less than -1 * previous right bearing.
		// In that cases the _width gets _smaller_ after moving to the next block.
		//
		// But when we layout block and we're sure that _maxWidth is enough
		// for all the blocks to fit on their line we check each block, even the
		// intermediate one with a large negative right bearing.
		accumulate_max(maxWidth, width);

		width += last_rBearing + (last_rPadding + b->f_width() - b__f_rbearing);
		lineHeight = qMax(lineHeight, blockHeight);

		last_rBearing = b__f_rbearing;
		last_rPadding = b->f_rpadding();
		continue;
	}
	if (initial) {
		computeParagraphDirection(_text.size());
	}
	if (width > 0) {
		if (!lineHeight) {
			lineHeight = CountBlockHeight(_blocks.back().get(), _st);
		}
		_minHeight += ppadding.top() + lineHeight + ppadding.bottom();
		accumulate_max(maxWidth, width);
	}
	_maxWidth = maxWidth.ceil().toInt();
	_endsWithParagraphDetails = (pindex != 0);
}

int String::countMaxMonospaceWidth() const {
	auto result = QFixed();
	auto paragraphWidth = QFixed();
	auto fullMonospace = true;
	QFixed _width = 0, last_rBearing = 0, last_rPadding = 0;
	for (auto &block : _blocks) {
		auto b = block.get();
		auto _btype = b->type();
		if (_btype == TextBlockType::Newline) {
			last_rBearing = b->f_rbearing();
			last_rPadding = b->f_rpadding();

			if (fullMonospace) {
				accumulate_max(paragraphWidth, _width);
				accumulate_max(result, paragraphWidth);
				paragraphWidth = 0;
			} else {
				fullMonospace = true;
			}
			_width = (b->f_width() - last_rBearing);
			continue;
		}
		if (!(b->flags() & (TextBlockFlag::Pre | TextBlockFlag::Code))
			&& (b->type() != TextBlockType::Skip)) {
			fullMonospace = false;
		}
		auto b__f_rbearing = b->f_rbearing(); // cache

		// We need to accumulate max width after each block, because
		// some blocks have width less than -1 * previous right bearing.
		// In that cases the _width gets _smaller_ after moving to the next block.
		//
		// But when we layout block and we're sure that _maxWidth is enough
		// for all the blocks to fit on their line we check each block, even the
		// intermediate one with a large negative right bearing.
		if (fullMonospace) {
			accumulate_max(paragraphWidth, _width);
		}
		_width += last_rBearing + (last_rPadding + b->f_width() - b__f_rbearing);

		last_rBearing = b__f_rbearing;
		last_rPadding = b->f_rpadding();
		continue;
	}
	if (_width > 0 && fullMonospace) {
		accumulate_max(paragraphWidth, _width);
		accumulate_max(result, paragraphWidth);
	}
	return result.ceil().toInt();
}

void String::setMarkedText(const style::TextStyle &st, const TextWithEntities &textWithEntities, const TextParseOptions &options, const std::any &context) {
	_st = &st;
	clear();
	{
		// utf codes of the text display for emoji extraction
//		auto text = textWithEntities.text;
//		auto newText = QString();
//		newText.reserve(8 * text.size());
//		newText.append("\t{ ");
//		for (const QChar *ch = text.constData(), *e = ch + text.size(); ch != e; ++ch) {
//			if (*ch == TextCommand) {
//				break;
//			} else if (IsNewline(*ch)) {
//				newText.append("},").append(*ch).append("\t{ ");
//			} else {
//				if (ch->isHighSurrogate() || ch->isLowSurrogate()) {
//					if (ch->isHighSurrogate() && (ch + 1 != e) && ((ch + 1)->isLowSurrogate())) {
//						newText.append("0x").append(QString::number((uint32(ch->unicode()) << 16) | uint32((ch + 1)->unicode()), 16).toUpper()).append("U, ");
//						++ch;
//					} else {
//						newText.append("BADx").append(QString::number(ch->unicode(), 16).toUpper()).append("U, ");
//					}
//				} else {
//					newText.append("0x").append(QString::number(ch->unicode(), 16).toUpper()).append("U, ");
//				}
//			}
//		}
//		newText.append("},\n\n").append(text);
//		Parser parser(this, { newText, EntitiesInText() }, options, context);

		Parser parser(this, textWithEntities, options, context);
	}
	recountNaturalSize(true, options.dir);
}

void String::setLink(uint16 index, const ClickHandlerPtr &link) {
	const auto extended = _extended.get();
	if (extended && index > 0 && index <= extended->links.size()) {
		extended->links[index - 1] = link;
	}
}

void String::setSpoilerRevealed(bool revealed, anim::type animated) {
	const auto data = _extended ? _extended->spoiler.get() : nullptr;
	if (!data) {
		return;
	} else if (data->revealed == revealed) {
		if (animated == anim::type::instant
			&& data->revealAnimation.animating()) {
			data->revealAnimation.stop();
			data->animation.repaintCallback()();
		}
		return;
	}
	data->revealed = revealed;
	if (animated == anim::type::instant) {
		data->revealAnimation.stop();
		data->animation.repaintCallback()();
	} else {
		data->revealAnimation.start(
			data->animation.repaintCallback(),
			revealed ? 0. : 1.,
			revealed ? 1. : 0.,
			st::fadeWrapDuration);
	}
}

void String::setSpoilerLinkFilter(Fn<bool(const ClickContext&)> filter) {
	Expects(_extended && _extended->spoiler);

	_extended->spoiler->link = std::make_shared<SpoilerClickHandler>(
		this,
		std::move(filter));
}

bool String::hasLinks() const {
	return _extended && !_extended->links.empty();
}

bool String::hasSpoilers() const {
	return _extended && (_extended->spoiler != nullptr);
}

bool String::hasSkipBlock() const {
	return !_blocks.empty()
		&& (_blocks.back()->type() == TextBlockType::Skip);
}

bool String::updateSkipBlock(int width, int height) {
	if (!_blocks.empty() && _blocks.back()->type() == TextBlockType::Skip) {
		const auto block = static_cast<SkipBlock*>(_blocks.back().get());
		if (block->f_width().toInt() == width && block->height() == height) {
			return false;
		}
		_text.resize(block->position());
		_blocks.pop_back();
	} else if (_endsWithParagraphDetails) {
		_text.push_back(QChar::LineFeed);
		_blocks.push_back(Block::Newline(
			_st->font,
			_text,
			_text.size() - 1,
			1,
			0,
			0,
			0));
		_skipBlockAddedNewline = true;
	}
	_text.push_back('_');
	_blocks.push_back(Block::Skip(
		_st->font,
		_text,
		_text.size() - 1,
		width,
		height,
		0,
		0));
	recountNaturalSize(false);
	return true;
}

bool String::removeSkipBlock() {
	if (_blocks.empty() || _blocks.back()->type() != TextBlockType::Skip) {
		return false;
	} else if (_skipBlockAddedNewline) {
		_text.resize(_blocks.back()->position() - 1);
		_blocks.pop_back();
		_blocks.pop_back();
		_skipBlockAddedNewline = false;
	} else {
		_text.resize(_blocks.back()->position());
		_blocks.pop_back();
	}
	recountNaturalSize(false);
	return true;
}

int String::countWidth(int width, bool breakEverywhere) const {
	if (QFixed(width) >= _maxWidth) {
		return _maxWidth;
	}

	QFixed maxLineWidth = 0;
	enumerateLines(width, breakEverywhere, [&](QFixed lineWidth, int lineHeight) {
		if (lineWidth > maxLineWidth) {
			maxLineWidth = lineWidth;
		}
	});
	return maxLineWidth.ceil().toInt();
}

int String::countHeight(int width, bool breakEverywhere) const {
	if (QFixed(width) >= _maxWidth) {
		return _minHeight;
	}
	int result = 0;
	enumerateLines(width, breakEverywhere, [&](QFixed lineWidth, int lineHeight) {
		result += lineHeight;
	});
	return result;
}

std::vector<int> String::countLineWidths(int width) const {
	return countLineWidths(width, {});
}

std::vector<int> String::countLineWidths(
		int width,
		LineWidthsOptions options) const {
	auto result = std::vector<int>();
	if (options.reserve) {
		result.reserve(options.reserve);
	}
	enumerateLines(width, options.breakEverywhere, [&](QFixed lineWidth, int lineHeight) {
		result.push_back(lineWidth.ceil().toInt());
	});
	return result;
}

template <typename Callback>
void String::enumerateLines(
		int w,
		bool breakEverywhere,
		Callback callback) const {
	const auto width = QFixed(std::max(w, _minResizeWidth));

	auto pindex = paragraphIndex(nullptr);
	auto paragraph = paragraphByIndex(pindex);
	auto ppadding = paragraphPadding(paragraph);
	auto widthLeft = width - ppadding.left() - ppadding.right();
	auto lineHeight = 0;
	auto last_rBearing = QFixed();
	auto last_rPadding = QFixed();
	bool longWordLine = true;
	for (auto &b : _blocks) {
		auto _btype = b->type();
		const auto blockHeight = CountBlockHeight(b.get(), _st);

		if (_btype == TextBlockType::Newline) {
			if (!lineHeight) {
				lineHeight = blockHeight;
			}
			lineHeight += ppadding.top();
			const auto index = paragraphIndex(b.get());
			if (pindex != index) {
				lineHeight += ppadding.bottom();
				pindex = index;
				paragraph = paragraphByIndex(pindex);
				ppadding = paragraphPadding(paragraph);
			} else {
				ppadding.setTop(0);
			}

			callback(width - widthLeft, lineHeight);

			lineHeight = 0;
			last_rBearing = 0;// b->f_rbearing(); (0 for newline)
			last_rPadding = 0;// b->f_rpadding(); (0 for newline)
			widthLeft = width - ppadding.left() - ppadding.right();
			// - (b->f_width() - last_rBearing); (0 for newline)

			longWordLine = true;
			continue;
		}
		auto b__f_rbearing = b->f_rbearing();
		auto newWidthLeft = widthLeft - last_rBearing - (last_rPadding + b->f_width() - b__f_rbearing);
		if (newWidthLeft >= 0) {
			last_rBearing = b__f_rbearing;
			last_rPadding = b->f_rpadding();
			widthLeft = newWidthLeft;

			lineHeight = qMax(lineHeight, blockHeight);

			longWordLine = false;
			continue;
		}

		if (_btype == TextBlockType::Text) {
			const auto t = &b.unsafe<TextBlock>();
			if (t->_words.isEmpty()) { // no words in this block, spaces only => layout this block in the same line
				last_rPadding += b->f_rpadding();

				lineHeight = qMax(lineHeight, blockHeight);

				longWordLine = false;
				continue;
			}

			auto f_wLeft = widthLeft;
			int f_lineHeight = lineHeight;
			for (auto j = t->_words.cbegin(), e = t->_words.cend(), f = j; j != e; ++j) {
				bool wordEndsHere = (j->f_width() >= 0);
				auto j_width = wordEndsHere ? j->f_width() : -j->f_width();

				auto newWidthLeft = widthLeft - last_rBearing - (last_rPadding + j_width - j->f_rbearing());
				if (newWidthLeft >= 0) {
					last_rBearing = j->f_rbearing();
					last_rPadding = j->f_rpadding();
					widthLeft = newWidthLeft;

					lineHeight = qMax(lineHeight, blockHeight);

					if (wordEndsHere) {
						longWordLine = false;
					}
					if (wordEndsHere || longWordLine) {
						f_wLeft = widthLeft;
						f_lineHeight = lineHeight;
						f = j + 1;
					}
					continue;
				}

				if (f != j && !breakEverywhere) {
					j = f;
					widthLeft = f_wLeft;
					lineHeight = f_lineHeight;
					j_width = (j->f_width() >= 0) ? j->f_width() : -j->f_width();
				}

				callback(width - widthLeft, lineHeight + ppadding.top());
				ppadding.setTop(0);

				lineHeight = qMax(0, blockHeight);
				last_rBearing = j->f_rbearing();
				last_rPadding = j->f_rpadding();
				widthLeft = width
					- ppadding.left()
					- ppadding.right()
					- (j_width - last_rBearing);

				longWordLine = !wordEndsHere;
				f = j + 1;
				f_wLeft = widthLeft;
				f_lineHeight = lineHeight;
			}
			continue;
		}

		callback(width - widthLeft, lineHeight + ppadding.top());
		ppadding.setTop(0);

		lineHeight = qMax(0, blockHeight);
		last_rBearing = b__f_rbearing;
		last_rPadding = b->f_rpadding();
		widthLeft = width
			- ppadding.left()
			- ppadding.right()
			- (b->f_width() - last_rBearing);

		longWordLine = true;
		continue;
	}
	if (widthLeft < width) {
		callback(
			width - widthLeft,
			lineHeight + ppadding.top() + ppadding.bottom());
	}
}

void String::draw(QPainter &p, const PaintContext &context) const {
	Renderer(*this).draw(p, context);
}

StateResult String::getState(
		QPoint point,
		GeometryDescriptor geometry,
		StateRequest request) const {
	return Renderer(*this).getState(point, std::move(geometry), request);
}

void String::draw(Painter &p, int32 left, int32 top, int32 w, style::align align, int32 yFrom, int32 yTo, TextSelection selection, bool fullWidthSelection) const {
//	p.fillRect(QRect(left, top, w, countHeight(w)), QColor(0, 0, 0, 32)); // debug
	Renderer(*this).draw(p, {
		.position = { left, top },
		.availableWidth = w,
		.align = align,
		.clip = (yTo >= 0
			? QRect(left, top + yFrom, w, yTo - yFrom)
			: QRect()),
		.palette = &p.textPalette(),
		.paused = p.inactive(),
		.selection = selection,
		.fullWidthSelection = fullWidthSelection,
	});
}

void String::drawElided(Painter &p, int32 left, int32 top, int32 w, int32 lines, style::align align, int32 yFrom, int32 yTo, int32 removeFromEnd, bool breakEverywhere, TextSelection selection) const {
//	p.fillRect(QRect(left, top, w, countHeight(w)), QColor(0, 0, 0, 32)); // debug
	Renderer(*this).draw(p, {
		.position = { left, top },
		.availableWidth = w,
		.align = align,
		.clip = (yTo >= 0
			? QRect(left, top + yFrom, w, yTo - yFrom)
			: QRect()),
		.palette = &p.textPalette(),
		.paused = p.inactive(),
		.selection = selection,
		.elisionHeight = ((!isEmpty() && lines > 1)
			? (lines * _st->font->height)
			: 0),
		.elisionRemoveFromEnd = removeFromEnd,
		.elisionOneLine = (lines == 1),
	});
}

void String::drawLeft(Painter &p, int32 left, int32 top, int32 width, int32 outerw, style::align align, int32 yFrom, int32 yTo, TextSelection selection) const {
	Renderer(*this).draw(p, {
		.position = { left, top },
		//.outerWidth = outerw,
		.availableWidth = width,
		.align = align,
		.clip = (yTo >= 0
			? QRect(left, top + yFrom, width, yTo - yFrom)
			: QRect()),
		.palette = &p.textPalette(),
		.paused = p.inactive(),
		.selection = selection,
	});
}

void String::drawLeftElided(Painter &p, int32 left, int32 top, int32 width, int32 outerw, int32 lines, style::align align, int32 yFrom, int32 yTo, int32 removeFromEnd, bool breakEverywhere, TextSelection selection) const {
	drawElided(p, style::RightToLeft() ? (outerw - left - width) : left, top, width, lines, align, yFrom, yTo, removeFromEnd, breakEverywhere, selection);
}

void String::drawRight(Painter &p, int32 right, int32 top, int32 width, int32 outerw, style::align align, int32 yFrom, int32 yTo, TextSelection selection) const {
	drawLeft(p, (outerw - right - width), top, width, outerw, align, yFrom, yTo, selection);
}

void String::drawRightElided(Painter &p, int32 right, int32 top, int32 width, int32 outerw, int32 lines, style::align align, int32 yFrom, int32 yTo, int32 removeFromEnd, bool breakEverywhere, TextSelection selection) const {
	drawLeftElided(p, (outerw - right - width), top, width, outerw, lines, align, yFrom, yTo, removeFromEnd, breakEverywhere, selection);
}

StateResult String::getState(QPoint point, int width, StateRequest request) const {
	if (isEmpty()) {
		return {};
	}
	return Renderer(*this).getState(
		point,
		SimpleGeometry(width, _st->font->height, 0, 0, false, false),
		request);
}

StateResult String::getStateLeft(QPoint point, int width, int outerw, StateRequest request) const {
	return getState(style::rtlpoint(point, outerw), width, request);
}

StateResult String::getStateElided(QPoint point, int width, StateRequestElided request) const {
	if (isEmpty()) {
		return {};
	}
	return Renderer(*this).getState(point, SimpleGeometry(
		width,
		_st->font->height,
		(request.lines > 1) ? (request.lines * _st->font->height) : 0,
		request.removeFromEnd,
		(request.lines == 1),
		request.flags & StateRequest::Flag::BreakEverywhere
	), static_cast<StateRequest>(request));
}

StateResult String::getStateElidedLeft(QPoint point, int width, int outerw, StateRequestElided request) const {
	return getStateElided(style::rtlpoint(point, outerw), width, request);
}

TextSelection String::adjustSelection(TextSelection selection, TextSelectType selectType) const {
	uint16 from = selection.from, to = selection.to;
	if (from < _text.size() && from <= to) {
		if (to > _text.size()) to = _text.size();
		if (selectType == TextSelectType::Paragraphs) {

			// Full selection of monospace entity.
			for (const auto &b : _blocks) {
				if (b->position() < from) {
					continue;
				}
				if (!IsMono(b->flags())) {
					break;
				}
				const auto &entities = toTextWithEntities().entities;
				const auto eIt = ranges::find_if(entities, [&](
						const EntityInText &e) {
					return (e.type() == EntityType::Pre
							|| e.type() == EntityType::Code)
						&& (from >= e.offset())
						&& ((e.offset() + e.length()) >= to);
				});
				if (eIt != entities.end()) {
					from = eIt->offset();
					to = eIt->offset() + eIt->length();
					while (to > 0 && IsSpace(_text.at(to - 1))) {
						--to;
					}
					if (to >= from) {
						return { from, to };
					}
				}
				break;
			}

			if (!IsParagraphSeparator(_text.at(from))) {
				while (from > 0 && !IsParagraphSeparator(_text.at(from - 1))) {
					--from;
				}
			}
			if (to < _text.size()) {
				if (IsParagraphSeparator(_text.at(to))) {
					++to;
				} else {
					while (to < _text.size() && !IsParagraphSeparator(_text.at(to))) {
						++to;
					}
				}
			}
		} else if (selectType == TextSelectType::Words) {
			if (!IsWordSeparator(_text.at(from))) {
				while (from > 0 && !IsWordSeparator(_text.at(from - 1))) {
					--from;
				}
			}
			if (to < _text.size()) {
				if (IsWordSeparator(_text.at(to))) {
					++to;
				} else {
					while (to < _text.size() && !IsWordSeparator(_text.at(to))) {
						++to;
					}
				}
			}
		}
	}
	return { from, to };
}

bool String::isEmpty() const {
	return _blocks.empty() || _blocks[0]->type() == TextBlockType::Skip;
}

not_null<ExtendedData*> String::ensureExtended() {
	if (!_extended) {
		_extended = std::make_unique<ExtendedData>();
	}
	return _extended.get();
}

uint16 String::countBlockEnd(
		const TextBlocks::const_iterator &i,
		const TextBlocks::const_iterator &e) const {
	return (i + 1 == e) ? _text.size() : (*(i + 1))->position();
}

uint16 String::countBlockLength(
		const TextBlocks::const_iterator &i,
		const TextBlocks::const_iterator &e) const {
	return countBlockEnd(i, e) - (*i)->position();
}

ParagraphDetails *String::paragraphByIndex(int index) const {
	Expects(!index
		|| (_extended && index <= _extended->paragraphs.size()));

	return index ? &_extended->paragraphs[index - 1] : nullptr;
}

int String::paragraphIndex(const AbstractBlock *block) const {
	Expects(!block || block->type() == TextBlockType::Newline);

	return block
		? static_cast<const NewlineBlock*>(block)->paragraphIndex()
		: _startParagraphIndex;
}

QMargins String::paragraphPadding(ParagraphDetails *info) const {
	if (!info) {
		return {};
	}
	const auto &st = info->pre ? _st->pre : _st->blockquote;
	const auto skip = st.verticalSkip;
	const auto top = st.header;
	return st.padding + QMargins(0, top + skip, 0, skip);
}

template <
	typename AppendPartCallback,
	typename ClickHandlerStartCallback,
	typename ClickHandlerFinishCallback,
	typename FlagsChangeCallback>
void String::enumerateText(
		TextSelection selection,
		AppendPartCallback appendPartCallback,
		ClickHandlerStartCallback clickHandlerStartCallback,
		ClickHandlerFinishCallback clickHandlerFinishCallback,
		FlagsChangeCallback flagsChangeCallback) const {
	if (isEmpty() || selection.empty()) {
		return;
	}

	int linkIndex = 0;
	uint16 linkPosition = 0;

	int32 flags = 0;
	for (auto i = _blocks.cbegin(), e = _blocks.cend(); true; ++i) {
		const auto blockPosition = (i == e) ? uint16(_text.size()) : (*i)->position();
		const auto blockFlags = (i == e) ? TextBlockFlags() : (*i)->flags();
		const auto blockLinkIndex = [&] {
			if (IsMono(blockFlags) || (i == e)) {
				return 0;
			}
			const auto result = (*i)->linkIndex();
			return (result && _extended && _extended->links[result - 1])
				? result
				: 0;
		}();
		if (blockLinkIndex != linkIndex) {
			if (linkIndex) {
				auto rangeFrom = qMax(selection.from, linkPosition);
				auto rangeTo = qMin(selection.to, blockPosition);
				if (rangeTo > rangeFrom) { // handle click handler
					const auto r = base::StringViewMid(_text, rangeFrom, rangeTo - rangeFrom);
					// Ignore links that are partially copied.
					const auto handler = (linkPosition != rangeFrom
						|| blockPosition != rangeTo
						|| !_extended)
						? nullptr
						: _extended->links[linkIndex - 1];
					const auto type = handler
						? handler->getTextEntity().type
						: EntityType::Invalid;
					clickHandlerFinishCallback(r, handler, type);
				}
			}
			linkIndex = blockLinkIndex;
			if (linkIndex) {
				linkPosition = blockPosition;
				const auto handler = _extended
					? _extended->links[linkIndex - 1]
					: nullptr;
				clickHandlerStartCallback(handler
					? handler->getTextEntity().type
					: EntityType::Invalid);
			}
		}

		const auto checkBlockFlags = (blockPosition >= selection.from)
			&& (blockPosition <= selection.to);
		if (checkBlockFlags && blockFlags != flags) {
			flagsChangeCallback(flags, blockFlags);
			flags = blockFlags;
		}
		if (i == e || (linkIndex ? linkPosition : blockPosition) >= selection.to) {
			break;
		}

		const auto blockType = (*i)->type();
		if (blockType == TextBlockType::Skip) {
			continue;
		}

		auto rangeFrom = qMax(selection.from, blockPosition);
		auto rangeTo = qMin(
			selection.to,
			uint16(blockPosition + countBlockLength(i, e)));
		if (rangeTo > rangeFrom) {
			const auto customEmojiData = (blockType == TextBlockType::CustomEmoji)
				? static_cast<const CustomEmojiBlock*>(i->get())->_custom->entityData()
				: QString();
			appendPartCallback(
				base::StringViewMid(_text, rangeFrom, rangeTo - rangeFrom),
				customEmojiData);
		}
	}
}

bool String::hasPersistentAnimation() const {
	return _hasCustomEmoji || hasSpoilers();
}

void String::unloadPersistentAnimation() {
	if (_hasCustomEmoji) {
		for (const auto &block : _blocks) {
			const auto raw = block.get();
			if (raw->type() == TextBlockType::CustomEmoji) {
				static_cast<const CustomEmojiBlock*>(raw)->_custom->unload();
			}
		}
	}
}

bool String::isOnlyCustomEmoji() const {
	return _isOnlyCustomEmoji;
}

OnlyCustomEmoji String::toOnlyCustomEmoji() const {
	if (!_isOnlyCustomEmoji) {
		return {};
	}
	auto result = OnlyCustomEmoji();
	result.lines.emplace_back();
	for (const auto &block : _blocks) {
		const auto raw = block.get();
		if (raw->type() == TextBlockType::CustomEmoji) {
			const auto custom = static_cast<const CustomEmojiBlock*>(raw);
			result.lines.back().push_back({
				.entityData = custom->_custom->entityData(),
			});
		} else if (raw->type() == TextBlockType::Newline) {
			result.lines.emplace_back();
		}
	}
	return result;
}

bool String::hasNotEmojiAndSpaces() const {
	return _hasNotEmojiAndSpaces;
}

const std::vector<Modification> &String::modifications() const {
	static const auto kEmpty = std::vector<Modification>();
	return _extended ? _extended->modifications : kEmpty;
}

QString String::toString(TextSelection selection) const {
	return toText(selection, false, false).rich.text;
}

TextWithEntities String::toTextWithEntities(TextSelection selection) const {
	return toText(selection, false, true).rich;
}

TextForMimeData String::toTextForMimeData(TextSelection selection) const {
	return toText(selection, true, true);
}

TextForMimeData String::toText(
		TextSelection selection,
		bool composeExpanded,
		bool composeEntities) const {
	struct MarkdownTagTracker {
		TextBlockFlags flag = TextBlockFlags();
		EntityType type = EntityType();
		int start = 0;
	};
	auto result = TextForMimeData();
	result.rich.text.reserve(_text.size());
	if (composeExpanded) {
		result.expanded.reserve(_text.size());
	}
	const auto insertEntity = [&](EntityInText &&entity) {
		auto i = result.rich.entities.end();
		while (i != result.rich.entities.begin()) {
			auto j = i;
			if ((--j)->offset() <= entity.offset()) {
				break;
			}
			i = j;
		}
		result.rich.entities.insert(i, std::move(entity));
	};
	auto linkStart = 0;
	auto markdownTrackers = composeEntities
		? std::vector<MarkdownTagTracker>{
			{ TextBlockFlag::Italic, EntityType::Italic },
			{ TextBlockFlag::Bold, EntityType::Bold },
			{ TextBlockFlag::Semibold, EntityType::Semibold },
			{ TextBlockFlag::Underline, EntityType::Underline },
			{ TextBlockFlag::Spoiler, EntityType::Spoiler },
			{ TextBlockFlag::StrikeOut, EntityType::StrikeOut },
			{ TextBlockFlag::Code, EntityType::Code }, // #TODO entities
			{ TextBlockFlag::Pre, EntityType::Pre },
			{ TextBlockFlag::Blockquote, EntityType::Blockquote },
		} : std::vector<MarkdownTagTracker>();
	const auto flagsChangeCallback = [&](int32 oldFlags, int32 newFlags) {
		if (!composeEntities) {
			return;
		}
		for (auto &tracker : markdownTrackers) {
			const auto flag = tracker.flag;
			if ((oldFlags & flag) && !(newFlags & flag)) {
				insertEntity({
					tracker.type,
					tracker.start,
					int(result.rich.text.size()) - tracker.start });
			} else if ((newFlags & flag) && !(oldFlags & flag)) {
				tracker.start = result.rich.text.size();
			}
		}
	};
	const auto clickHandlerStartCallback = [&](EntityType type) {
		linkStart = result.rich.text.size();
	};
	const auto clickHandlerFinishCallback = [&](
			QStringView inText,
			const ClickHandlerPtr &handler,
			EntityType type) {
		if (!handler || (!composeExpanded && !composeEntities)) {
			return;
		}
		// This logic is duplicated in TextForMimeData::WithExpandedLinks.
		const auto entity = handler->getTextEntity();
		const auto plainUrl = (entity.type == EntityType::Url)
			|| (entity.type == EntityType::Email);
		const auto full = plainUrl
			? QStringView(entity.data).mid(0, entity.data.size())
			: inText;
		const auto customTextLink = (entity.type == EntityType::CustomUrl);
		const auto internalLink = customTextLink
			&& entity.data.startsWith(qstr("internal:"));
		if (composeExpanded) {
			const auto sameAsTextLink = customTextLink
				&& (entity.data
					== UrlClickHandler::EncodeForOpening(full.toString()));
			if (customTextLink && !internalLink && !sameAsTextLink) {
				const auto &url = entity.data;
				result.expanded.append(qstr(" (")).append(url).append(')');
			}
		}
		if (composeEntities && !internalLink) {
			insertEntity({
				entity.type,
				linkStart,
				int(result.rich.text.size() - linkStart),
				plainUrl ? QString() : entity.data });
		}
	};
	const auto appendPartCallback = [&](
			QStringView part,
			const QString &customEmojiData) {
		result.rich.text += part;
		if (composeExpanded) {
			result.expanded += part;
		}
		if (composeEntities && !customEmojiData.isEmpty()) {
			insertEntity({
				EntityType::CustomEmoji,
				int(result.rich.text.size() - part.size()),
				int(part.size()),
				customEmojiData,
			});
		}
	};

	enumerateText(
		selection,
		appendPartCallback,
		clickHandlerStartCallback,
		clickHandlerFinishCallback,
		flagsChangeCallback);

	if (composeEntities) {
		const auto proj = [](const EntityInText &entity) {
			const auto type = entity.type();
			const auto isUrl = (type == EntityType::Url)
				|| (type == EntityType::CustomUrl)
				|| (type == EntityType::BotCommand)
				|| (type == EntityType::Mention)
				|| (type == EntityType::MentionName)
				|| (type == EntityType::Hashtag)
				|| (type == EntityType::Cashtag);
			return std::pair{ entity.offset(), isUrl ? 0 : 1 };
		};
		const auto pred = [&](const EntityInText &a, const EntityInText &b) {
			return proj(a) < proj(b);
		};
		std::sort(
			result.rich.entities.begin(),
			result.rich.entities.end(),
			pred);
	}

	return result;
}

bool String::isIsolatedEmoji() const {
	return _isIsolatedEmoji;
}

IsolatedEmoji String::toIsolatedEmoji() const {
	if (!_isIsolatedEmoji) {
		return {};
	}
	auto result = IsolatedEmoji();
	const auto skip = (_blocks.empty()
		|| _blocks.back()->type() != TextBlockType::Skip) ? 0 : 1;
	if ((_blocks.size() > kIsolatedEmojiLimit + skip) || hasSpoilers()) {
		return {};
	}
	auto index = 0;
	for (const auto &block : _blocks) {
		const auto type = block->type();
		if (block->linkIndex()) {
			return {};
		} else if (type == TextBlockType::Emoji) {
			result.items[index++] = block.unsafe<EmojiBlock>()._emoji;
		} else if (type == TextBlockType::CustomEmoji) {
			result.items[index++]
				= block.unsafe<CustomEmojiBlock>()._custom->entityData();
		} else if (type != TextBlockType::Skip) {
			return {};
		}
	}
	return result;
}

void String::clear() {
	_text.clear();
	_blocks.clear();
	_extended = nullptr;
	_maxWidth = _minHeight = 0;
	_startParagraphIndex = 0;
	_startParagraphLTR = false;
	_startParagraphRTL = false;
}

bool IsBad(QChar ch) {
	return (ch == 0)
		|| (ch >= 8232 && ch < 8237)
		|| (ch >= 65024 && ch < 65040 && ch != 65039)
		|| (ch >= 127 && ch < 160 && ch != 156)

		// qt harfbuzz crash see https://github.com/telegramdesktop/tdesktop/issues/4551
		|| (Platform::IsMac() && ch == 6158);
}

bool IsWordSeparator(QChar ch) {
	switch (ch.unicode()) {
	case QChar::Space:
	case QChar::LineFeed:
	case '.':
	case ',':
	case '?':
	case '!':
	case '@':
	case '#':
	case '$':
	case ':':
	case ';':
	case '-':
	case '<':
	case '>':
	case '[':
	case ']':
	case '(':
	case ')':
	case '{':
	case '}':
	case '=':
	case '/':
	case '+':
	case '%':
	case '&':
	case '^':
	case '*':
	case '\'':
	case '"':
	case '`':
	case '~':
	case '|':
		return true;
	default:
		break;
	}
	return false;
}

bool IsAlmostLinkEnd(QChar ch) {
	switch (ch.unicode()) {
	case '?':
	case ',':
	case '.':
	case '"':
	case ':':
	case '!':
	case '\'':
		return true;
	default:
		break;
	}
	return false;
}

bool IsLinkEnd(QChar ch) {
	return IsBad(ch)
		|| IsSpace(ch)
		|| IsNewline(ch)
		|| ch.isLowSurrogate()
		|| ch.isHighSurrogate();
}

bool IsNewline(QChar ch) {
	return (ch == QChar::LineFeed)
		|| (ch == 156);
}

bool IsSpace(QChar ch) {
	return ch.isSpace()
		|| (ch < 32)
		|| (ch == QChar::ParagraphSeparator)
		|| (ch == QChar::LineSeparator)
		|| (ch == QChar::ObjectReplacementCharacter)
		|| (ch == QChar::CarriageReturn)
		|| (ch == QChar::Tabulation)
		|| (ch == QChar(8203)/*Zero width space.*/);
}

bool IsDiacritic(QChar ch) { // diacritic and variation selectors
	return (ch.category() == QChar::Mark_NonSpacing)
		|| (ch == 1652)
		|| (ch >= 64606 && ch <= 64611);
}

bool IsReplacedBySpace(QChar ch) {
	// \xe2\x80[\xa8 - \xac\xad] // 8232 - 8237
	// QString from1 = QString::fromUtf8("\xe2\x80\xa8"), to1 = QString::fromUtf8("\xe2\x80\xad");
	// \xcc[\xb3\xbf\x8a] // 819, 831, 778
	// QString bad1 = QString::fromUtf8("\xcc\xb3"), bad2 = QString::fromUtf8("\xcc\xbf"), bad3 = QString::fromUtf8("\xcc\x8a");
	// [\x00\x01\x02\x07\x08\x0b-\x1f] // '\t' = 0x09
	return (/*code >= 0x00 && */ch <= 0x02)
		|| (ch >= 0x07 && ch <= 0x09)
		|| (ch >= 0x0b && ch <= 0x1f)
		|| (ch == 819)
		|| (ch == 831)
		|| (ch == 778)
		|| (ch >= 8232 && ch <= 8237);
}

bool IsTrimmed(QChar ch) {
	return (IsSpace(ch) || IsBad(ch));
}

} // namespace Ui::Text
