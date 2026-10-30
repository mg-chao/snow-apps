// SPDX-License-Identifier: GPL-3.0-only
#include "snow_shot/presentation/imagesplitdocument.h"

#include <QCoreApplication>
#include <QStringView>

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <optional>
#include <unordered_set>
#include <utility>

namespace snow_shot::presentation {

struct ImageSplitLines::Data final {
    struct Run final {
        int start = 0;
        qint64 step = 0;
        qsizetype count = 0;
    };
    using Root = std::shared_ptr<const Data>;

    Run run;
    Root left;
    Root right;
    qsizetype count = 0;
    qsizetype nodes = 0;
    int height = 0;
    int first = 0;
    int last = 0;
    bool sorted = true;
    quint64 sum = 0;
    quint64 weightedSum = 0;

    static qsizetype countOf(const Root& root) {
        return root ? root->count : 0;
    }
    static qsizetype nodesOf(const Root& root) {
        return root ? root->nodes : 0;
    }
    static int heightOf(const Root& root) {
        return root ? root->height : 0;
    }
    static bool endpoint(const Run& run, qint64& result) {
        if (run.count <= 0)
            return false;
        const qint64 steps = run.count - 1;
        if (run.step > 0 &&
            steps > (qint64(std::numeric_limits<int>::max()) - run.start) / run.step)
            return false;
        if (run.step < 0 &&
            steps > (qint64(run.start) - std::numeric_limits<int>::min()) / -run.step)
            return false;
        result = qint64(run.start) + steps * run.step;
        return true;
    }
    static int coordinateAt(const Run& run, qsizetype index) {
        return static_cast<int>(qint64(run.start) + qint64(index) * run.step);
    }
    static quint64 indexSum(quint64 count) {
        if (count == 0)
            return 0;
        return count % 2 == 0 ? (count / 2) * (count - 1) : count * ((count - 1) / 2);
    }
    static quint64 indexSquareSum(quint64 count) {
        if (count == 0)
            return 0;
        // Divide before unsigned multiplication so the result remains the exact sum modulo 2^64.
        std::array<quint64, 3> factors{count, count - 1, 2 * count - 1};
        for (auto& factor : factors)
            if (factor % 2 == 0) {
                factor /= 2;
                break;
            }
        for (auto& factor : factors)
            if (factor % 3 == 0) {
                factor /= 3;
                break;
            }
        return factors[0] * factors[1] * factors[2];
    }
    Data(Run value, Root before, Root after)
        : run(value), left(std::move(before)), right(std::move(after)) {
        Q_ASSERT(heightOf(left) <= heightOf(right) + 1 && heightOf(right) <= heightOf(left) + 1);
        if (run.count == 1)
            run.step = 0;
        qint64 runLast = 0;
        const bool valid = endpoint(run, runLast);
        Q_ASSERT(valid);
        Q_UNUSED(valid);
        count = countOf(left) + run.count + countOf(right);
        nodes = nodesOf(left) + 1 + nodesOf(right);
        height = 1 + std::max(heightOf(left), heightOf(right));
        first = left ? left->first : run.start;
        last = right ? right->last : static_cast<int>(runLast);
        sorted = (run.count == 1 || run.step > 0) &&
                 (!left || (left->sorted && left->last < run.start)) &&
                 (!right || (right->sorted && runLast < right->first));
        const quint64 length = static_cast<quint64>(run.count);
        const quint64 runSum = static_cast<quint64>(run.start) * length +
                               static_cast<quint64>(run.step) * indexSum(length);
        const quint64 runWeighted = static_cast<quint64>(run.start) * indexSum(length) +
                                    static_cast<quint64>(run.step) * indexSquareSum(length);
        sum = (left ? left->sum : 0) + runSum + (right ? right->sum : 0);
        weightedSum = (left ? left->weightedSum : 0) + runWeighted +
                      static_cast<quint64>(countOf(left)) * runSum +
                      (right ? right->weightedSum +
                                   static_cast<quint64>(countOf(left) + run.count) * right->sum
                             : 0);
    }
    static Root make(Run run, Root left = {}, Root right = {}) {
        return std::make_shared<Data>(run, std::move(left), std::move(right));
    }
    static Root balanced(Run run, const Root& left, const Root& right) {
        if (heightOf(left) > heightOf(right) + 1) {
            if (heightOf(left->left) >= heightOf(left->right))
                return make(left->run, left->left, make(run, left->right, right));
            const auto& middle = left->right;
            return make(middle->run, make(left->run, left->left, middle->left),
                        make(run, middle->right, right));
        }
        if (heightOf(right) > heightOf(left) + 1) {
            if (heightOf(right->right) >= heightOf(right->left))
                return make(right->run, make(run, left, right->left), right->right);
            const auto& middle = right->left;
            return make(middle->run, make(run, left, middle->left),
                        make(right->run, middle->right, right->right));
        }
        return make(run, left, right);
    }
    static Root join(const Root& left, Run run, const Root& right) {
        if (heightOf(left) > heightOf(right) + 1)
            return balanced(left->run, left->left, join(left->right, run, right));
        if (heightOf(right) > heightOf(left) + 1)
            return balanced(right->run, join(left, run, right->left), right->right);
        return make(run, left, right);
    }
    static const Run& runAt(const Root& root, qsizetype index) {
        const Data* node = root.get();
        while (node) {
            const qsizetype before = nodesOf(node->left);
            if (index < before)
                node = node->left.get();
            else if (index == before)
                return node->run;
            else {
                index -= before + 1;
                node = node->right.get();
            }
        }
        Q_UNREACHABLE();
    }
    static std::pair<Root, Run> popLast(const Root& root) {
        if (!root->right)
            return {root->left, root->run};
        auto [remaining, run] = popLast(root->right);
        return {join(root->left, root->run, remaining), run};
    }
    static std::pair<Run, Root> popFirst(const Root& root) {
        if (!root->left)
            return {root->run, root->right};
        auto [run, remaining] = popFirst(root->left);
        return {run, join(remaining, root->run, root->right)};
    }
    static std::optional<Run> merged(Run first, Run second) {
        const qint64 gap = qint64(second.start) - coordinateAt(first, first.count - 1);
        const qint64 step = first.count == 1 ? gap : first.step;
        if (gap != step || (second.count > 1 && second.step != step))
            return std::nullopt;
        return Run{first.start, step, first.count + second.count};
    }
    static Root concat(const Root& left, const Root& right) {
        if (!left)
            return right;
        if (!right)
            return left;
        if (const auto run = merged(runAt(left, left->nodes - 1), runAt(right, 0))) {
            const auto before = popLast(left).first;
            const auto after = popFirst(right).second;
            return join(before, *run, after);
        }
        auto [run, after] = popFirst(right);
        return join(left, run, after);
    }
    static Root coalesceLast(const Root& root) {
        if (!root || root->nodes < 2)
            return root;
        const auto run = merged(runAt(root, root->nodes - 2), runAt(root, root->nodes - 1));
        if (!run)
            return root;
        return join(popLast(popLast(root).first).first, *run, {});
    }
    static Root coalesceFirst(const Root& root) {
        if (!root || root->nodes < 2)
            return root;
        const auto run = merged(runAt(root, 0), runAt(root, 1));
        if (!run)
            return root;
        return join({}, *run, popFirst(popFirst(root).second).second);
    }
    static std::pair<Root, Root> splitRaw(const Root& root, qsizetype index) {
        if (index == 0)
            return {{}, root};
        if (index == countOf(root))
            return {root, {}};
        const qsizetype before = countOf(root->left);
        if (index < before) {
            auto [first, second] = splitRaw(root->left, index);
            return {first, join(second, root->run, root->right)};
        }
        if (index > before + root->run.count) {
            auto [first, second] = splitRaw(root->right, index - before - root->run.count);
            return {join(root->left, root->run, first), second};
        }
        const qsizetype offset = index - before;
        const Root first = offset == 0
                               ? root->left
                               : join(root->left, {root->run.start, root->run.step, offset}, {});
        const Root second =
            offset == root->run.count
                ? root->right
                : join({},
                       {coordinateAt(root->run, offset), root->run.step, root->run.count - offset},
                       root->right);
        return {first, second};
    }
    static std::pair<Root, Root> split(const Root& root, qsizetype index) {
        auto [first, second] = splitRaw(root, index);
        // Only the new end fragments can acquire a compatible neighboring run.
        return {coalesceLast(first), coalesceFirst(second)};
    }
    static Root build(const QVector<Run>& runs, qsizetype first, qsizetype count) {
        if (count == 0)
            return {};
        const qsizetype middle = count / 2;
        return make(runs.at(first + middle), build(runs, first, middle),
                    build(runs, first + middle + 1, count - middle - 1));
    }
    template <typename Values> static Root fromValues(const Values& values) {
        QVector<Run> runs;
        for (const int value : values) {
            const Run next{value, 0, 1};
            const auto run = runs.isEmpty() ? std::optional<Run>{} : merged(runs.last(), next);
            if (run)
                runs.last() = *run;
            else
                runs.append(next);
        }
        return build(runs, 0, runs.size());
    }
    class RunCursor final {
      public:
        explicit RunCursor(const Root& root) {
            descend(root.get());
        }
        const Run* next() {
            if (m_depth == 0)
                return nullptr;
            const Data* node = m_stack[--m_depth];
            descend(node->right.get());
            return &node->run;
        }

      private:
        void descend(const Data* node) {
            while (node) {
                Q_ASSERT(m_depth < m_stack.size());
                m_stack[m_depth++] = node;
                node = node->left.get();
            }
        }
        // An AVL tree whose guide count fits qsizetype cannot approach this depth.
        std::array<const Data*, 128> m_stack{};
        size_t m_depth = 0;
    };
    static qsizetype uniqueBytes(const Root& root, std::unordered_set<const Data*>& seen) {
        if (!root || !seen.insert(root.get()).second)
            return 0;
        return qsizetype(sizeof(Data)) + uniqueBytes(root->left, seen) +
               uniqueBytes(root->right, seen);
    }
};

ImageSplitLines::ImageSplitLines(std::initializer_list<int> values)
    : m_data(Data::fromValues(values)) {}
ImageSplitLines::ImageSplitLines(const QVector<int>& values) : m_data(Data::fromValues(values)) {}
qsizetype ImageSplitLines::size() const {
    return Data::countOf(m_data);
}
bool ImageSplitLines::isEmpty() const {
    return size() == 0;
}
int ImageSplitLines::at(qsizetype index) const {
    Q_ASSERT(index >= 0 && index < size());
    if (index < 0 || index >= size())
        return 0;
    const Data* node = m_data.get();
    while (node) {
        const qsizetype before = Data::countOf(node->left);
        if (index < before)
            node = node->left.get();
        else if (index < before + node->run.count)
            return Data::coordinateAt(node->run, index - before);
        else {
            index -= before + node->run.count;
            node = node->right.get();
        }
    }
    Q_UNREACHABLE();
}
int ImageSplitLines::operator[](qsizetype index) const {
    return at(index);
}
ImageSplitLines::Reference ImageSplitLines::operator[](qsizetype index) {
    return Reference(this, index);
}
int ImageSplitLines::first() const {
    Q_ASSERT(m_data);
    return m_data ? m_data->first : 0;
}
int ImageSplitLines::last() const {
    Q_ASSERT(m_data);
    return m_data ? m_data->last : 0;
}
bool ImageSplitLines::contains(int coordinate) const {
    if (m_data && !m_data->sorted) {
        Data::RunCursor cursor(m_data);
        while (const auto* run = cursor.next()) {
            const qint64 difference = qint64(coordinate) - run->start;
            if (run->step == 0) {
                if (difference == 0)
                    return true;
            } else if (difference % run->step == 0 && difference / run->step >= 0 &&
                       difference / run->step < run->count) {
                return true;
            }
        }
        return false;
    }
    const qsizetype index = coordinateLowerBound(coordinate);
    return index < size() && at(index) == coordinate;
}
qsizetype ImageSplitLines::coordinateLowerBound(double coordinate) const {
    Q_ASSERT(!m_data || m_data->sorted);
    const Data* node = m_data.get();
    qsizetype index = 0;
    while (node) {
        if (node->left && !(node->left->last < coordinate)) {
            node = node->left.get();
            continue;
        }
        index += Data::countOf(node->left);
        const auto& run = node->run;
        if (Data::coordinateAt(run, run.count - 1) < coordinate) {
            index += run.count;
            node = node->right.get();
            continue;
        }
        if (!(run.start < coordinate))
            return index;
        qsizetype offset = static_cast<qsizetype>((coordinate - run.start) / double(run.step));
        offset = std::min(offset, run.count - 1);
        while (offset < run.count && Data::coordinateAt(run, offset) < coordinate)
            ++offset;
        while (offset > 0 && !(Data::coordinateAt(run, offset - 1) < coordinate))
            --offset;
        return index + offset;
    }
    return index;
}
qsizetype ImageSplitLines::coordinateUpperBound(double coordinate) const {
    Q_ASSERT(!m_data || m_data->sorted);
    const Data* node = m_data.get();
    qsizetype index = 0;
    while (node) {
        if (node->left && coordinate < node->left->last) {
            node = node->left.get();
            continue;
        }
        index += Data::countOf(node->left);
        const auto& run = node->run;
        if (!(coordinate < Data::coordinateAt(run, run.count - 1))) {
            index += run.count;
            node = node->right.get();
            continue;
        }
        if (coordinate < run.start)
            return index;
        qsizetype offset = static_cast<qsizetype>((coordinate - run.start) / double(run.step));
        offset = std::min(offset, run.count - 1);
        while (offset < run.count && !(coordinate < Data::coordinateAt(run, offset)))
            ++offset;
        while (offset > 0 && coordinate < Data::coordinateAt(run, offset - 1))
            --offset;
        return index + offset;
    }
    return index;
}
ImageSplitLines::const_iterator ImageSplitLines::begin() const {
    return {this, 0};
}
ImageSplitLines::const_iterator ImageSplitLines::end() const {
    return {this, size()};
}
ImageSplitLines::const_iterator ImageSplitLines::cbegin() const {
    return begin();
}
ImageSplitLines::const_iterator ImageSplitLines::cend() const {
    return end();
}
void ImageSplitLines::reserve(qsizetype count) {
    Q_UNUSED(count);
}
void ImageSplitLines::append(int coordinate) {
    insert(size(), coordinate);
}
void ImageSplitLines::insert(qsizetype index, int coordinate) {
    Q_ASSERT(index >= 0 && index <= size());
    if (index < 0 || index > size() || size() == std::numeric_limits<qsizetype>::max())
        return;
    const auto [first, second] = Data::split(m_data, index);
    m_data = Data::concat(Data::concat(first, Data::make({coordinate, 0, 1})), second);
}
ImageSplitLines::const_iterator ImageSplitLines::insert(const_iterator position, int coordinate) {
    Q_ASSERT(position.m_owner == this);
    insert(position.m_index, coordinate);
    return {this, position.m_index};
}
void ImageSplitLines::removeAt(qsizetype index) {
    Q_ASSERT(index >= 0 && index < size());
    if (index < 0 || index >= size())
        return;
    const auto [first, tail] = Data::split(m_data, index);
    const auto second = Data::split(tail, 1).second;
    m_data = Data::concat(first, second);
}
void ImageSplitLines::clear() {
    m_data.reset();
}
void ImageSplitLines::setAt(qsizetype index, int coordinate) {
    Q_ASSERT(index >= 0 && index < size());
    if (index < 0 || index >= size() || at(index) == coordinate)
        return;
    const auto [first, tail] = Data::split(m_data, index);
    const auto second = Data::split(tail, 1).second;
    m_data = Data::concat(Data::concat(first, Data::make({coordinate, 0, 1})), second);
}
bool ImageSplitLines::appendArithmetic(int start, int step, qsizetype count) {
    if (count == 0)
        return true;
    Data::Run run{start, step, count};
    qint64 last = 0;
    if (count < 0 || count > std::numeric_limits<qsizetype>::max() - size() ||
        !Data::endpoint(run, last))
        return false;
    m_data = Data::concat(m_data, Data::make(run));
    return true;
}
bool ImageSplitLines::isValid(int dimension) const {
    return dimension > 0 && size() < dimension &&
           (!m_data || (m_data->sorted && m_data->first > 0 && m_data->last < dimension));
}
qsizetype ImageSplitLines::runCount() const {
    return Data::nodesOf(m_data);
}
qsizetype ImageSplitLines::storageBytes() const {
    return runCount() * qsizetype(sizeof(Data));
}
bool operator==(const ImageSplitLines& first, const ImageSplitLines& second) {
    if (first.m_data == second.m_data)
        return true;
    if (first.size() != second.size())
        return false;
    if (first.isEmpty())
        return true;
    // Signatures only reject unequal axes. Equal signatures always receive an exact run comparison.
    if (first.m_data->sum != second.m_data->sum ||
        first.m_data->weightedSum != second.m_data->weightedSum)
        return false;
    ImageSplitLines::Data::RunCursor aCursor(first.m_data);
    ImageSplitLines::Data::RunCursor bCursor(second.m_data);
    const auto* a = aCursor.next();
    const auto* b = bCursor.next();
    qsizetype aOffset = 0;
    qsizetype bOffset = 0;
    while (a && b) {
        const qsizetype overlap = std::min(a->count - aOffset, b->count - bOffset);
        if (ImageSplitLines::Data::coordinateAt(*a, aOffset) !=
                ImageSplitLines::Data::coordinateAt(*b, bOffset) ||
            (overlap > 1 && a->step != b->step))
            return false;
        aOffset += overlap;
        bOffset += overlap;
        if (aOffset == a->count) {
            a = aCursor.next();
            aOffset = 0;
        }
        if (bOffset == b->count) {
            b = bCursor.next();
            bOffset = 0;
        }
    }
    return true;
}
bool operator==(const ImageSplitLines& first, const QVector<int>& second) {
    if (first.size() != second.size())
        return false;
    ImageSplitLines::Data::RunCursor cursor(first.m_data);
    qsizetype index = 0;
    while (const auto* run = cursor.next())
        for (qsizetype offset = 0; offset < run->count; ++offset)
            if (ImageSplitLines::Data::coordinateAt(*run, offset) != second.at(index++))
                return false;
    return true;
}
ImageSplitLines::Reference::operator int() const {
    return m_owner->at(m_index);
}
ImageSplitLines::Reference& ImageSplitLines::Reference::operator=(int value) {
    m_owner->setAt(m_index, value);
    return *this;
}
ImageSplitLines::Reference&
ImageSplitLines::Reference::operator=(const ImageSplitLines::Reference& other) {
    return operator=(int(other));
}
int ImageSplitLines::const_iterator::operator*() const {
    return m_owner->at(m_index);
}
int ImageSplitLines::const_iterator::operator[](difference_type offset) const {
    return m_owner->at(m_index + offset);
}
ImageSplitLines::const_iterator& ImageSplitLines::const_iterator::operator++() {
    ++m_index;
    return *this;
}
ImageSplitLines::const_iterator ImageSplitLines::const_iterator::operator++(int) {
    const auto previous = *this;
    ++*this;
    return previous;
}
ImageSplitLines::const_iterator& ImageSplitLines::const_iterator::operator--() {
    --m_index;
    return *this;
}
ImageSplitLines::const_iterator ImageSplitLines::const_iterator::operator--(int) {
    const auto previous = *this;
    --*this;
    return previous;
}
ImageSplitLines::const_iterator&
ImageSplitLines::const_iterator::operator+=(difference_type offset) {
    m_index += offset;
    return *this;
}
ImageSplitLines::const_iterator&
ImageSplitLines::const_iterator::operator-=(difference_type offset) {
    m_index -= offset;
    return *this;
}

namespace {
[[maybe_unused]] constexpr const char* labels[] = {
    QT_TRANSLATE_NOOP("ImageSplitDocument", "Enter a positive whole number."),
    QT_TRANSLATE_NOOP("ImageSplitDocument", "Each image must be at least one pixel wide or high."),
    QT_TRANSLATE_NOOP("ImageSplitDocument", "Enter at least 16 pixels."),
    QT_TRANSLATE_NOOP("ImageSplitDocument", "Choose pixels or percentage."),
    QT_TRANSLATE_NOOP("ImageSplitDocument", "Enter a percentage from 5% to 100%."),
    QT_TRANSLATE_NOOP("ImageSplitDocument", "The image is unavailable."),
    QT_TRANSLATE_NOOP("ImageSplitDocument", "Choose a split type."),
    QT_TRANSLATE_NOOP("ImageSplitDocument", "Choose a split direction."),
    QT_TRANSLATE_NOOP("ImageSplitDocument", "The split lines require too much memory."),
};
QString splitText(const char* value) {
    return QCoreApplication::translate("ImageSplitDocument", value);
}

QString normalizedDigits(QString value) {
    qsizetype first = 0;
    while (first + 1 < value.size() && value.at(first) == u'0')
        ++first;
    return value.sliced(first);
}

// Decimal multiplication keeps percentage boundaries exact even when the entered decimal is a
// binary floating-point half-pixel near miss. The multiplier is at most INT_MAX * 20.
QString multiplyDigits(const QString& digits, quint64 multiplier) {
    QString result(digits.size() + 20, u'0');
    qsizetype destination = result.size();
    quint64 carry = 0;
    for (qsizetype index = digits.size(); index > 0; --index) {
        const quint64 product =
            quint64(digits.at(index - 1).unicode() - ushort('0')) * multiplier + carry;
        result[--destination] = QChar(ushort('0') + ushort(product % 10));
        carry = product / 10;
    }
    while (carry > 0) {
        result[--destination] = QChar(ushort('0') + ushort(carry % 10));
        carry /= 10;
    }
    return normalizedDigits(result.sliced(destination));
}

int compareDigits(const QString& first, const QString& second) {
    if (first.size() != second.size())
        return first.size() < second.size() ? -1 : 1;
    return QString::compare(first, second, Qt::CaseSensitive);
}

struct DecimalPercentage final {
    QString digits;
    qsizetype fractionalDigits = 0;

    QString denominator() const {
        return QStringLiteral("100") + QString(fractionalDigits, u'0');
    }
};

std::optional<DecimalPercentage> parseDecimalPercentage(QString value) {
    value = value.trimmed();
    // Input text has its own bound; geometry and exports have no arbitrary output-count limit.
    if (value.isEmpty() || value.size() > 4096)
        return {};
    if (value.startsWith(u'+'))
        value.remove(0, 1);
    const auto separator = value.indexOf(u'.');
    QString whole = separator < 0 ? value : value.first(separator);
    QString fractional = separator < 0 ? QString() : value.sliced(separator + 1);
    const auto decimalDigits = [](QStringView text) {
        return std::all_of(text.begin(), text.end(),
                           [](QChar character) { return character >= u'0' && character <= u'9'; });
    };
    if (whole.isEmpty() || !decimalDigits(whole) || !decimalDigits(fractional))
        return {};
    whole = normalizedDigits(std::move(whole));
    while (fractional.endsWith(u'0'))
        fractional.chop(1);
    return DecimalPercentage{normalizedDigits(whole + fractional), fractional.size()};
}

std::optional<DecimalPercentage> parsePercentage(const QString& value) {
    const auto result = parseDecimalPercentage(value);
    if (!result ||
        compareDigits(result->digits,
                      QStringLiteral("5") + QString(result->fractionalDigits, u'0')) < 0 ||
        compareDigits(result->digits, result->denominator()) > 0)
        return {};
    return result;
}

std::optional<int> percentageBoundary(const DecimalPercentage& percentage, int dimension,
                                      int multiple) {
    const QString product =
        multiplyDigits(percentage.digits, quint64(dimension) * quint64(multiple));
    const qsizetype scale = percentage.fractionalDigits + 2;
    if (product.size() < scale)
        return 0;
    const qsizetype integerDigits = product.size() - scale;
    const QString whole = integerDigits == 0 ? QStringLiteral("0") : product.first(integerDigits);
    if (compareDigits(whole, QString::number(std::numeric_limits<int>::max())) > 0)
        return {};
    const qint64 integer = whole.toLongLong() + (product.at(integerDigits) >= u'5' ? 1 : 0);
    return integer > std::numeric_limits<int>::max()
               ? std::optional<int>{}
               : std::optional<int>{static_cast<int>(integer)};
}

bool equalLines(int dimension, int count, const QString& field, ImageSplitLayout& layout,
                ImageSplitLines& destination) {
    if (count <= 0) {
        layout.errors.insert(field, splitText("Enter a positive whole number."));
        return false;
    }
    if (count > dimension) {
        layout.errors.insert(field,
                             splitText("Each image must be at least one pixel wide or high."));
        return false;
    }
    if (count == 1)
        return true;
    const int section = dimension / count;
    const int extra = dimension % count;
    const qsizetype leading = std::min(extra, count - 1);
    const qsizetype remaining = count - 1 - leading;
    if (!destination.appendArithmetic(section + 1, section + 1, leading) ||
        !destination.appendArithmetic(static_cast<int>(qint64(extra) * (section + 1) + section),
                                      section, remaining))
        return false;
    return true;
}

bool preciseLines(int dimension, const ImageSplitSettings& settings, ImageSplitLayout& layout,
                  ImageSplitLines& destination) {
    if (settings.unit == ImageSplitUnit::Pixels) {
        if (settings.pixels < 16) {
            layout.errors.insert(QStringLiteral("pixels"), splitText("Enter at least 16 pixels."));
            return false;
        }
        return destination.appendArithmetic(settings.pixels, settings.pixels,
                                            (dimension - 1) / settings.pixels);
    }
    if (settings.unit != ImageSplitUnit::Percentage) {
        layout.errors.insert(QStringLiteral("percentage"),
                             splitText("Choose pixels or percentage."));
        return false;
    }
    const auto percentage = parsePercentage(settings.percentage);
    if (!percentage) {
        layout.errors.insert(QStringLiteral("percentage"),
                             splitText("Enter a percentage from 5% to 100%."));
        return false;
    }
    const QString denominator = percentage->denominator();
    if (compareDigits(multiplyDigits(percentage->digits, quint64(dimension)), denominator) < 0) {
        layout.errors.insert(QStringLiteral("percentage"),
                             splitText("Each image must be at least one pixel wide or high."));
        return false;
    }
    for (int multiple = 1; multiple <= 20; ++multiple) {
        if (compareDigits(multiplyDigits(percentage->digits, quint64(multiple)), denominator) >= 0)
            break;
        const auto boundary = percentageBoundary(*percentage, dimension, multiple);
        if (!boundary || *boundary >= dimension) {
            layout.errors.insert(QStringLiteral("percentage"),
                                 splitText("Each image must be at least one pixel wide or high."));
            return false;
        }
        const int coordinate = *boundary;
        if (coordinate <= 0 || (!destination.isEmpty() && coordinate <= destination.last())) {
            layout.errors.insert(QStringLiteral("percentage"),
                                 splitText("Each image must be at least one pixel wide or high."));
            return false;
        }
        destination.append(coordinate);
    }
    return true;
}
} // namespace

std::optional<int> imageSplitPercentagePixels(int dimension, const QString& percentage) {
    if (dimension <= 0)
        return {};
    const auto value = parseDecimalPercentage(percentage);
    return value ? percentageBoundary(*value, dimension, 1) : std::optional<int>{};
}

bool ImageSplitLayout::isValid() const {
    return size.isValid() && !size.isEmpty() && errors.isEmpty() &&
           verticalLines.isValid(size.width()) && horizontalLines.isValid(size.height());
}

qint64 ImageSplitLayout::outputCount() const {
    if (size.isEmpty() || !errors.isEmpty() || verticalLines.size() >= size.width() ||
        horizontalLines.size() >= size.height())
        return 0;
    return (qint64(verticalLines.size()) + 1) * (qint64(horizontalLines.size()) + 1);
}

QRect ImageSplitLayout::regionAt(qint64 index) const {
    if (index < 0 || index >= outputCount())
        return {};
    const qint64 columnCount = qint64(verticalLines.size()) + 1;
    const qsizetype column = static_cast<qsizetype>(index % columnCount);
    const qsizetype row = static_cast<qsizetype>(index / columnCount);
    const int left = column == 0 ? 0 : verticalLines.at(column - 1);
    const int right = column == verticalLines.size() ? size.width() : verticalLines.at(column);
    const int top = row == 0 ? 0 : horizontalLines.at(row - 1);
    const int bottom = row == horizontalLines.size() ? size.height() : horizontalLines.at(row);
    return QRect(left, top, right - left, bottom - top);
}

ImageSplitLayout calculateImageSplitLayout(QSize size, const ImageSplitSettings& settings) {
    ImageSplitLayout layout;
    layout.size = size;
    if (size.isEmpty()) {
        layout.errors.insert(QStringLiteral("image"), splitText("The image is unavailable."));
        return layout;
    }
    try {
        if (settings.direction == ImageSplitDirection::Grid) {
            equalLines(size.width(), settings.columns, QStringLiteral("columns"), layout,
                       layout.verticalLines);
            equalLines(size.height(), settings.rows, QStringLiteral("rows"), layout,
                       layout.horizontalLines);
        } else if (settings.direction == ImageSplitDirection::Vertical ||
                   settings.direction == ImageSplitDirection::Horizontal) {
            const bool vertical = settings.direction == ImageSplitDirection::Vertical;
            const int dimension = vertical ? size.width() : size.height();
            auto& lines = vertical ? layout.verticalLines : layout.horizontalLines;
            if (settings.type == ImageSplitType::Equal)
                equalLines(dimension, settings.count, QStringLiteral("count"), layout, lines);
            else if (settings.type == ImageSplitType::Precise)
                preciseLines(dimension, settings, layout, lines);
            else
                layout.errors.insert(QStringLiteral("type"), splitText("Choose a split type."));
        } else {
            layout.errors.insert(QStringLiteral("direction"),
                                 splitText("Choose a split direction."));
        }
    } catch (const std::bad_alloc&) {
        layout.verticalLines = {};
        layout.horizontalLines = {};
        layout.errors.insert(QStringLiteral("image"),
                             splitText("The split lines require too much memory."));
    }
    return layout;
}

struct ImageSplitDocument::Impl final {
    struct HistoryEntry final {
        ImageSplitState state;
        QMap<QString, QString> errors;
    };

    QImage image;
    QImage preview;
    ImageSplitState state;
    ImageSplitLayout layout;
    QVector<HistoryEntry> history;
    qsizetype cursor = 0;
    std::optional<ImageSplitState> beforeDrag;
    bool dragVertical = true;
    int dragIndex = -1;
    int dragStartIndex = -1;

    bool allows(bool vertical) const {
        return state.settings.direction == ImageSplitDirection::Grid ||
               (vertical && state.settings.direction == ImageSplitDirection::Vertical) ||
               (!vertical && state.settings.direction == ImageSplitDirection::Horizontal);
    }

    void synchronizeLines() {
        layout.verticalLines = state.verticalLines;
        layout.horizontalLines = state.horizontalLines;
    }

    void restoreHistory() {
        const auto& entry = history.at(cursor);
        state = entry.state;
        layout.size = image.size();
        layout.errors = entry.errors;
        synchronizeLines();
    }

    bool commit(ImageSplitState next) {
        if (next == state)
            return false;
        if (cursor + 1 < history.size())
            history.resize(cursor + 1);
        history.append({next, layout.errors});
        cursor = history.size() - 1;
        state = std::move(next);
        synchronizeLines();
        return true;
    }
};

ImageSplitDocument::ImageSplitDocument(QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>()) {
    m_impl->layout = calculateImageSplitLayout({}, {});
    m_impl->history.append({m_impl->state, m_impl->layout.errors});
}
ImageSplitDocument::~ImageSplitDocument() = default;

const QImage& ImageSplitDocument::image() const {
    return m_impl->image;
}
const QImage& ImageSplitDocument::preview() const {
    return m_impl->preview;
}
void ImageSplitDocument::setImage(QImage image, QImage preview,
                                  ImageSplitSettings initialSettings) {
    auto& d = *m_impl;
    d.beforeDrag.reset();
    d.image = std::move(image);
    d.preview = preview.isNull() ? d.image : std::move(preview);
    d.state = {};
    d.state.settings = std::move(initialSettings);
    d.layout = calculateImageSplitLayout(d.image.size(), d.state.settings);
    d.state.verticalLines = d.layout.verticalLines;
    d.state.horizontalLines = d.layout.horizontalLines;
    // Initializer-list assignment reuses the old allocation. Source replacement also releases the
    // entry capacity accumulated by the previous image's unlimited undo history.
    d.history = QVector<Impl::HistoryEntry>{{d.state, d.layout.errors}};
    d.cursor = 0;
    emit changed();
    emit historyChanged();
}
ImageSplitSettings ImageSplitDocument::settings() const {
    return m_impl->state.settings;
}
ImageSplitState ImageSplitDocument::state() const {
    return m_impl->state;
}
ImageSplitLayout ImageSplitDocument::layout() const {
    return m_impl->layout;
}
qsizetype ImageSplitDocument::historyStorageBytes() const {
    std::unordered_set<const ImageSplitLines::Data*> seen;
    qsizetype bytes = m_impl->history.capacity() * qsizetype(sizeof(Impl::HistoryEntry));
    const auto include = [&seen, &bytes](const ImageSplitState& state) {
        bytes += ImageSplitLines::Data::uniqueBytes(state.verticalLines.m_data, seen);
        bytes += ImageSplitLines::Data::uniqueBytes(state.horizontalLines.m_data, seen);
    };
    include(m_impl->state);
    for (const auto& entry : m_impl->history)
        include(entry.state);
    if (m_impl->beforeDrag)
        include(*m_impl->beforeDrag);
    return bytes;
}
bool ImageSplitDocument::setSettings(const ImageSplitSettings& settings) {
    if (settings == m_impl->state.settings)
        return false;
    const auto next = calculateImageSplitLayout(m_impl->image.size(), settings);
    if (!next.isValid())
        return false;
    cancelLineDrag();
    m_impl->layout = next;
    if (!m_impl->commit({settings, next.verticalLines, next.horizontalLines, false}))
        return false;
    emit changed();
    emit historyChanged();
    return true;
}
bool ImageSplitDocument::canUndo() const {
    return !m_impl->beforeDrag && m_impl->cursor > 0;
}
bool ImageSplitDocument::canRedo() const {
    return !m_impl->beforeDrag && m_impl->cursor + 1 < m_impl->history.size();
}
void ImageSplitDocument::undo() {
    if (!canUndo())
        return;
    auto& d = *m_impl;
    --d.cursor;
    d.restoreHistory();
    emit changed();
    emit historyChanged();
}
void ImageSplitDocument::redo() {
    if (!canRedo())
        return;
    auto& d = *m_impl;
    ++d.cursor;
    d.restoreHistory();
    emit changed();
    emit historyChanged();
}
bool ImageSplitDocument::createLine(bool vertical, int coordinate) {
    auto& d = *m_impl;
    if (d.beforeDrag || !d.layout.isValid() || !d.allows(vertical))
        return false;
    const int dimension = vertical ? d.image.width() : d.image.height();
    if (coordinate <= 0 || coordinate >= dimension)
        return false;
    auto next = d.state;
    auto& lines = vertical ? next.verticalLines : next.horizontalLines;
    const qsizetype insertion = lines.coordinateLowerBound(coordinate);
    if (insertion != lines.size() && lines.at(insertion) == coordinate)
        return false;
    lines.insert(insertion, coordinate);
    next.manuallyEdited = true;
    d.commit(std::move(next));
    emit changed();
    emit historyChanged();
    return true;
}
bool ImageSplitDocument::removeLine(bool vertical, int index) {
    auto& d = *m_impl;
    if (d.beforeDrag || !d.layout.isValid() || !d.allows(vertical))
        return false;
    auto next = d.state;
    auto& lines = vertical ? next.verticalLines : next.horizontalLines;
    if (index < 0 || index >= lines.size())
        return false;
    lines.removeAt(index);
    next.manuallyEdited = true;
    d.commit(std::move(next));
    emit changed();
    emit historyChanged();
    return true;
}
bool ImageSplitDocument::beginLineDrag(bool vertical, int index) {
    auto& d = *m_impl;
    if (d.beforeDrag || !d.layout.isValid() || !d.allows(vertical))
        return false;
    const auto& lines = vertical ? d.state.verticalLines : d.state.horizontalLines;
    if (index < 0 || index >= lines.size())
        return false;
    d.beforeDrag = d.state;
    d.dragVertical = vertical;
    d.dragIndex = index;
    d.dragStartIndex = index;
    emit historyChanged();
    return true;
}
bool ImageSplitDocument::moveDraggedLine(int coordinate) {
    auto& d = *m_impl;
    if (!d.beforeDrag)
        return false;
    auto& lines = d.dragVertical ? d.state.verticalLines : d.state.horizontalLines;
    const int dimension = d.dragVertical ? d.image.width() : d.image.height();
    coordinate = std::clamp(coordinate, 1, dimension - 1);
    const int previousCoordinate = lines.at(d.dragIndex);
    if (previousCoordinate == coordinate)
        return false;
    const qsizetype insertion = lines.coordinateLowerBound(coordinate);
    // Cuts remain unique so preview and export never contain zero-width or zero-height regions.
    // Landing on an occupied pixel leaves the gesture active; the next move can cross that cut.
    if (insertion != lines.size() && lines.at(insertion) == coordinate)
        return false;
    int index = static_cast<int>(insertion);
    if (index > d.dragIndex)
        --index;
    // Keep axes sorted for hit testing and region generation, while tracking the dragged line's
    // new index rather than treating its original neighbors as movement limits.
    if (d.dragVertical)
        d.layout.verticalLines = {};
    else
        d.layout.horizontalLines = {};
    const bool indexChanged = index != d.dragIndex;
    if (indexChanged) {
        lines.removeAt(d.dragIndex);
        lines.insert(index, coordinate);
        d.dragIndex = index;
    } else {
        lines[d.dragIndex] = coordinate;
    }
    d.state.manuallyEdited = true;
    d.synchronizeLines();
    if (indexChanged)
        emit lineDragIndexChanged(d.dragIndex);
    emit lineMoved(d.dragVertical, previousCoordinate, coordinate);
    emit changed();
    return true;
}
void ImageSplitDocument::endLineDrag() {
    auto& d = *m_impl;
    if (!d.beforeDrag)
        return;
    auto next = d.state;
    const bool geometryChanged = next.verticalLines != d.beforeDrag->verticalLines ||
                                 next.horizontalLines != d.beforeDrag->horizontalLines;
    d.state = std::move(*d.beforeDrag);
    d.beforeDrag.reset();
    if (geometryChanged)
        d.commit(std::move(next));
    else
        d.synchronizeLines();
    emit changed();
    emit historyChanged();
}
void ImageSplitDocument::cancelLineDrag() {
    auto& d = *m_impl;
    if (!d.beforeDrag)
        return;
    d.state = std::move(*d.beforeDrag);
    d.beforeDrag.reset();
    const bool indexChanged = d.dragIndex != d.dragStartIndex;
    d.dragIndex = d.dragStartIndex;
    d.synchronizeLines();
    if (indexChanged)
        emit lineDragIndexChanged(d.dragIndex);
    emit changed();
    emit historyChanged();
}

} // namespace snow_shot::presentation
