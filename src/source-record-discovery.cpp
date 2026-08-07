#include "source-record-discovery.hpp"

#include <cstring>

namespace {

void EnumFilterCallback(obs_source_t *, obs_source_t *child, void *param)
{
	auto *out = static_cast<std::vector<obs_weak_source_t *> *>(param);
	if (strcmp(obs_source_get_id(child), "source_record_filter") == 0) {
		obs_weak_source_t *weak = obs_source_get_weak_source(child);
		if (weak)
			out->push_back(weak);
	}
}

bool EnumSourceCallback(void *param, obs_source_t *source)
{
	obs_source_enum_filters(source, EnumFilterCallback, param);
	return true;
}

} // namespace

std::vector<obs_weak_source_t *> DiscoverSourceRecordFilters()
{
	std::vector<obs_weak_source_t *> found;
	obs_enum_sources(EnumSourceCallback, &found);
	obs_enum_scenes(EnumSourceCallback, &found);
	return found;
}

QString FilterRowLabel(obs_source_t *filterSource)
{
	obs_source_t *parent = obs_filter_get_parent(filterSource);
	if (parent)
		return QString::fromUtf8(obs_source_get_name(parent)) + QStringLiteral(" - ") +
		       QString::fromUtf8(obs_source_get_name(filterSource));
	return QString::fromUtf8(obs_source_get_name(filterSource));
}
