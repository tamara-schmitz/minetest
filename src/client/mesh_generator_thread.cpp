/*
Minetest
Copyright (C) 2013, 2017 celeron55, Perttu Ahola <celeron55@gmail.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation; either version 2.1 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include "mesh_generator_thread.h"
#include "settings.h"
#include "profiler.h"
#include "client.h"
#include "mapblock.h"
#include "map.h"
#include "util/directiontables.h"

static class BlockPlaceholder {
public:
	MapNode data[MAP_BLOCKSIZE * MAP_BLOCKSIZE * MAP_BLOCKSIZE];

	BlockPlaceholder()
	{
		for (std::size_t i = 0; i < MAP_BLOCKSIZE * MAP_BLOCKSIZE * MAP_BLOCKSIZE; i++)
			data[i] = MapNode(CONTENT_IGNORE);
	}

} block_placeholder;
/*
	QueuedMeshUpdate
*/

QueuedMeshUpdate::~QueuedMeshUpdate()
{
	delete data;
}

/*
	MeshUpdateQueue
*/

MeshUpdateQueue::MeshUpdateQueue(Client *client):
	m_client(client)
{
	m_cache_enable_shaders = g_settings->getBool("enable_shaders");
	m_cache_smooth_lighting = g_settings->getBool("smooth_lighting");
	m_meshgen_block_cache_size = g_settings->getS32("meshgen_block_cache_size");
}

MeshUpdateQueue::~MeshUpdateQueue()
{
	MutexAutoLock lock(m_mutex);

	for (QueuedMeshUpdate *q : m_queue) {
		for (auto block : q->map_blocks)
			if (block)
				block->refDrop();
		delete q;
	}
}

bool MeshUpdateQueue::addBlock(Map *map, v3s16 p, bool ack_block_to_server, bool urgent)
{
	MapBlock *main_block = map->getBlockNoCreateNoEx(p);
	if (!main_block)
		return false;

	MutexAutoLock lock(m_mutex);

	// Mesh is placed at even positions at all coordinates
	// (every 8-th block) and will cover 8 blocks
	v3s16 mesh_position(p.X & ~1, p.Y & ~1, p.Z & ~1);
	/*
		Mark the block as urgent if requested
	*/
	if (urgent)
		m_urgents.insert(mesh_position);

	/*
		Find if block is already in queue.
		If it is, update the data and quit.
	*/
	for (QueuedMeshUpdate *q : m_queue) {
		if (q->p == mesh_position) {
			// NOTE: We are not adding a new position to the queue, thus
			//       refcount_from_queue stays the same.
			if(ack_block_to_server)
				q->ack_list.push_back(p);
			q->crack_level = m_client->getCrackLevel();
			q->crack_pos = m_client->getCrackPos();
			q->urgent |= urgent;
			for (std::size_t i = 0; i < q->map_blocks.size(); i++) {
				if (!q->map_blocks[i]) {
					MapBlock *block = map->getBlockNoCreateNoEx(q->p + g_64dirs[i]);
					if (block) {
						block->refGrab();
						q->map_blocks[i] = block;
					}
				}
			}
			return true;
		}
	}

	/*
		Make a list of blocks necessary for mesh generation and lock the blocks in memory.
	*/
	std::vector<MapBlock *> map_blocks;
	map_blocks.reserve(4*4*4);
	for (v3s16 dp : g_64dirs) {
		MapBlock *block = map->getBlockNoCreateNoEx(mesh_position + dp);
		map_blocks.push_back(block);
		if (block)
			block->refGrab();
	}

	/*
		Add the block
	*/
	QueuedMeshUpdate *q = new QueuedMeshUpdate;
	q->p = mesh_position;
	if(ack_block_to_server)
		q->ack_list.push_back(p);
	q->crack_level = m_client->getCrackLevel();
	q->crack_pos = m_client->getCrackPos();
	q->urgent = urgent;
	q->map_blocks = std::move(map_blocks);
	m_queue.push_back(q);

	return true;
}

// Returned pointer must be deleted
// Returns NULL if queue is empty
QueuedMeshUpdate *MeshUpdateQueue::pop()
{
	QueuedMeshUpdate *result = NULL;
	{
		MutexAutoLock lock(m_mutex);

		bool must_be_urgent = !m_urgents.empty();
		for (std::vector<QueuedMeshUpdate*>::iterator i = m_queue.begin();
				i != m_queue.end(); ++i) {
			QueuedMeshUpdate *q = *i;
			if (must_be_urgent && m_urgents.count(q->p) == 0)
				continue;
			// Make sure no two threads are processing the same mapblock, as that causes racing conditions
			if (m_inflight_blocks.find(q->p) != m_inflight_blocks.end())
				continue;
			m_queue.erase(i);
			m_urgents.erase(q->p);
			m_inflight_blocks.insert(q->p);
			result = q;
			break;
		}
	}

	if (result)
		fillDataFromMapBlocks(result);

	return result;
}

void MeshUpdateQueue::done(v3s16 pos)
{
	MutexAutoLock lock(m_mutex);
	m_inflight_blocks.erase(pos);
}


void MeshUpdateQueue::fillDataFromMapBlocks(QueuedMeshUpdate *q)
{
	MeshMakeData *data = new MeshMakeData(m_client, m_cache_enable_shaders);
	q->data = data;
	data->side_length = 2 * MAP_BLOCKSIZE;

	data->fillBlockDataBegin(q->p);

	for (std::size_t i = 0; i < 64; i++) {
		MapBlock *block = q->map_blocks[i];
		data->fillBlockData(g_64dirs[i], block ? block->getData() : block_placeholder.data);
	}

	data->setCrack(q->crack_level, q->crack_pos);
	data->setSmoothLighting(m_cache_smooth_lighting);
}

/*
	MeshUpdateWorkerThread
*/

MeshUpdateWorkerThread::MeshUpdateWorkerThread(MeshUpdateQueue *queue_in, MeshUpdateManager *manager, v3s16 *camera_offset) :
		UpdateThread("Mesh"), m_queue_in(queue_in), m_manager(manager), m_camera_offset(camera_offset)
{
	m_generation_interval = g_settings->getU16("mesh_generation_interval");
	m_generation_interval = rangelim(m_generation_interval, 0, 50);
}

void MeshUpdateWorkerThread::doUpdate()
{
	QueuedMeshUpdate *q;
	while ((q = m_queue_in->pop())) {
		if (m_generation_interval)
			sleep_ms(m_generation_interval);
		ScopeProfiler sp(g_profiler, "Client: Mesh making (sum)");

		MapBlockMesh *mesh_new = new MapBlockMesh(q->data, *m_camera_offset);

		

		MeshUpdateResult r;
		r.p = q->p;
		r.mesh = mesh_new;
		r.solid_sides = get_solid_sides(q->data);
		r.ack_list = std::move(q->ack_list);
		r.urgent = q->urgent;
		r.map_blocks = q->map_blocks;

		m_manager->putResult(r);
		m_queue_in->done(q->p);
		delete q;
	}
}

/*
	MeshUpdateManager
*/

MeshUpdateManager::MeshUpdateManager(Client *client):
	m_queue_in(client)
{
	int number_of_threads = rangelim(g_settings->getS32("mesh_generation_threads"), 0, 8);

	// Automatically use 33% of the system cores for mesh generation, max 4
	if (number_of_threads == 0)
		number_of_threads = MYMIN(4, Thread::getNumberOfProcessors() / 3);
	
	// use at least one thread
	number_of_threads = MYMAX(1, number_of_threads);
	infostream << "MeshUpdateManager: using " << number_of_threads << " threads" << std::endl;

	for (int i = 0; i < number_of_threads; i++)
		m_workers.push_back(std::make_unique<MeshUpdateWorkerThread>(&m_queue_in, this, &m_camera_offset));
}

void MeshUpdateManager::updateBlock(Map *map, v3s16 p, bool ack_block_to_server,
		bool urgent, bool update_neighbors)
{
	static thread_local const bool many_neighbors =
			g_settings->getBool("smooth_lighting")
			&& !g_settings->getFlag("performance_tradeoffs");
	if (!m_queue_in.addBlock(map, p, ack_block_to_server, urgent)) {
		warningstream << "Update requested for non-existent block at ("
				<< p.X << ", " << p.Y << ", " << p.Z << ")" << std::endl;
		return;
	}
	if (update_neighbors) {
		if (many_neighbors) {
			for (v3s16 dp : g_26dirs)
				m_queue_in.addBlock(map, p + dp, false, urgent);
		} else {
			for (v3s16 dp : g_6dirs)
				m_queue_in.addBlock(map, p + dp, false, urgent);
		}
	}
	deferUpdate();
}

void MeshUpdateManager::putResult(const MeshUpdateResult &result)
{
	if (result.urgent)
		m_queue_out_urgent.push_back(result);
	else
		m_queue_out.push_back(result);
}

bool MeshUpdateManager::getNextResult(MeshUpdateResult &r)
{
	if (!m_queue_out_urgent.empty()) {
		r = m_queue_out_urgent.pop_frontNoEx();
		return true;
	}

	if (!m_queue_out.empty()) {
		r = m_queue_out.pop_frontNoEx();
		return true;
	}

	return false;
}

void MeshUpdateManager::deferUpdate()
{
	for (auto &thread : m_workers)
		thread->deferUpdate();
}

void MeshUpdateManager::start()
{
	for (auto &thread: m_workers)
		thread->start();
}

void MeshUpdateManager::stop()
{
	for (auto &thread: m_workers)
		thread->stop();
}

void MeshUpdateManager::wait()
{
	for (auto &thread: m_workers)
		thread->wait();
}

bool MeshUpdateManager::isRunning()
{
	for (auto &thread: m_workers)
		if (thread->isRunning())
			return true;
	return false;
}
