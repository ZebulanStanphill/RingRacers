// The sidecar record of a copied game data file and the rule for using the copy (src/uwp_data_copy.hpp); runs without
// the engine.
#include "uwp_data_copy.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>

using namespace uwp_data_copy;

#define CHECK(cond) \
	do \
	{ \
		if (!(cond)) \
		{ \
			std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
			assert(false); \
			return 1; \
		} \
	} while (0)

static const char* kMd5 = "0123456789abcdef0123456789ABCDEF";
static const char* kMd5Lower = "0123456789abcdef0123456789abcdef";
static const char* kOtherMd5 = "ffffffffffffffffffffffffffffffff";

static bool parse(const std::string& s, Record& r)
{
	return ParseRecord(s.data(), s.size(), r);
}

static Record record(uint64_t size, int64_t modified, const char* md5)
{
	Record r;
	r.size = size;
	r.modified = modified;
	r.md5 = md5;
	return r;
}

static SourceStat source(uint64_t size, int64_t modified)
{
	SourceStat s;
	s.exists = true;
	s.size = size;
	s.modified = modified;
	return s;
}

static CopyStat copyOf(uint64_t size)
{
	CopyStat c;
	c.exists = true;
	c.size = size;
	return c;
}

static int testNormalize()
{
	std::string out;
	CHECK(NormalizeMd5(nullptr, out) && out == "-");
	CHECK(NormalizeMd5(kMd5, out) && out == kMd5Lower);
	CHECK(!NormalizeMd5("", out) && out.empty());
	CHECK(!NormalizeMd5("0123", out));
	CHECK(!NormalizeMd5("0123456789abcdef0123456789abcdeg", out));
	CHECK(!NormalizeMd5("0123456789abcdef0123456789abcdef0", out));
	CHECK(!NormalizeMd5("-", out));
	return 0;
}

static int testRoundTrip()
{
	const Record cases[] = {
		record(0, 0, "-"),
		record(311296000, 133700000000000000, kMd5Lower),
		record(UINT64_MAX, INT64_MAX, kOtherMd5),
		record(1, -5, "-"),
		record(1, INT64_MIN + 1, "-"),
	};
	for (const Record& in : cases)
	{
		const std::string text = FormatRecord(in);
		CHECK(text.back() == '\n' && text.find('\n') == text.size() - 1);
		Record out;
		CHECK(parse(text, out));
		CHECK(out.size == in.size && out.modified == in.modified && out.md5 == in.md5);
		// The newline is optional
		Record bare;
		CHECK(parse(text.substr(0, text.size() - 1), bare) && bare.size == in.size && bare.md5 == in.md5);
	}
	CHECK(FormatRecord(record(5, 6, "-")) == "ringracers-data-copy 1 5 6 -\n");
	return 0;
}

static int testRejectedRecords()
{
	Record r;
	const std::string good = FormatRecord(record(10, 20, kMd5Lower));
	CHECK(parse(good, r));

	CHECK(!ParseRecord(nullptr, 0, r));
	CHECK(!parse("", r));
	CHECK(!parse("\n", r));
	CHECK(!parse("other-magic 1 10 20 -\n", r));
	CHECK(!parse("ringracers-data-copy 2 10 20 -\n", r)); // another version
	CHECK(!parse("ringracers-data-copy 0 10 20 -\n", r));
	CHECK(!parse("ringracers-data-copy 1 10 20\n", r)); // no MD5
	CHECK(!parse("ringracers-data-copy 1 10 20 --\n", r));
	CHECK(!parse("ringracers-data-copy 1 -10 20 -\n", r)); // negative size
	CHECK(!parse("ringracers-data-copy 1 10 2x0 -\n", r));
	CHECK(!parse("ringracers-data-copy 1 18446744073709551616 20 -\n", r)); // size overflows
	CHECK(!parse("ringracers-data-copy 1 10 9223372036854775808 -\n", r)); // time overflows
	CHECK(!parse("ringracers-data-copy 1 10 20 0123456789ABCDEF0123456789ABCDEF\n", r)); // uppercase
	CHECK(!parse("ringracers-data-copy 1 10 20 0123456789abcdef\n", r)); // short MD5
	CHECK(!parse(good + good, r)); // two lines
	CHECK(!parse(good + "x", r));
	CHECK(!parse("ringracers-data-copy  1 10 20 -\n", r)); // double space
	CHECK(!parse(std::string(good.c_str(), good.size() - 1) + std::string(1, '\0') + "\n", r)); // embedded NUL
	CHECK(!parse(std::string(kMaxRecord, 'x'), r));

	// Every truncation of a valid record is rejected, or is the whole line without its newline
	for (size_t n = 0; n + 1 < good.size(); n++)
	{
		Record t;
		CHECK(!ParseRecord(good.data(), n, t));
	}
	return 0;
}

static int testDecide()
{
	const std::string md5 = kMd5Lower;
	const Record rec = record(1000, 77, kMd5Lower);

	CHECK(Decide(source(1000, 77), copyOf(1000), &rec, md5) == Verdict::Usable);

	SourceStat missing;
	CHECK(Decide(missing, copyOf(1000), &rec, md5) == Verdict::SourceMissing);
	CHECK(Decide(source(1000, 77), CopyStat(), &rec, md5) == Verdict::CopyMissing);
	CHECK(Decide(source(1000, 77), copyOf(1000), nullptr, md5) == Verdict::NoRecord);

	CHECK(Decide(source(1001, 77), copyOf(1000), &rec, md5) == Verdict::SourceChanged);
	CHECK(Decide(source(1000, 78), copyOf(1000), &rec, md5) == Verdict::SourceChanged);
	CHECK(Decide(source(1000, 76), copyOf(1000), &rec, md5) == Verdict::SourceChanged);

	CHECK(Decide(source(1000, 77), copyOf(1000), &rec, std::string(kOtherMd5)) == Verdict::Md5Changed);
	CHECK(Decide(source(1000, 77), copyOf(1000), &rec, std::string("-")) == Verdict::Md5Changed);

	CHECK(Decide(source(1000, 77), copyOf(999), &rec, md5) == Verdict::CopyIncomplete);
	CHECK(Decide(source(1000, 77), copyOf(1001), &rec, md5) == Verdict::CopyIncomplete);
	CHECK(Decide(source(1000, 77), copyOf(0), &rec, md5) == Verdict::CopyIncomplete);

	// Files with no expected MD5 are recorded with "-", and a later expected MD5 would not match
	const Record nomd5 = record(5, 6, "-");
	CHECK(Decide(source(5, 6), copyOf(5), &nomd5, "-") == Verdict::Usable);
	CHECK(Decide(source(5, 6), copyOf(5), &nomd5, md5) == Verdict::Md5Changed);

	// An empty file is a file
	const Record empty = record(0, 1, "-");
	CHECK(Decide(source(0, 1), copyOf(0), &empty, "-") == Verdict::Usable);

	// Checks come in a fixed order: the source decides before the record, the record before the copy's size
	CHECK(Decide(source(1001, 78), copyOf(5), &rec, std::string(kOtherMd5)) == Verdict::SourceChanged);
	CHECK(Decide(source(1000, 77), copyOf(5), &rec, std::string(kOtherMd5)) == Verdict::Md5Changed);

	CHECK(std::strcmp(VerdictText(Verdict::Usable), "usable") == 0);
	return 0;
}

int main()
{
	if (testNormalize() || testRoundTrip() || testRejectedRecords() || testDecide())
		return 1;
	std::puts("uwp_data_copy: ok");
	return 0;
}
