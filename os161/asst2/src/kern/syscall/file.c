#include <types.h>
#include <kern/errno.h>
#include <kern/fcntl.h>
#include <syscall.h>
#include <copyinout.h>
#include <machine/trapframe.h>
#include <kern/stat.h>
#include <kern/seek.h>
#include <limits.h>
#include <lib.h>
#include <uio.h>
#include <file.h>
#include <proc.h>
#include <thread.h>
#include <current.h>
#include <synch.h>
#include <vfs.h>
#include <vnode.h>


// TODO: Take in 3rd argument register and give to vfs_open (mode_t)
// Although this is not even implemented in OS161 so whatever...

static int open(char *filename, int flags, int file_descriptor);
static int total_file_opened=0;




/* open() function - called by user */
int sys_open(userptr_t filename, int flags, int *retVal) {

	size_t got;
	int err;
	int i=3;


	if (filename == NULL) {				//check if the filename pointer is valid
		err= EFAULT;
		return err;
	}
	char *kfilename = kmalloc((PATH_MAX)*sizeof(char));
	if (kfilename == NULL) {
		err=ENFILE;
		return err;
	}

	if (copyinstr(filename, kfilename, PATH_MAX, &got)) {			//copy the valid filename into the kernel buffer, using the copyinstr function
		kfree(kfilename);
		return copyinstr(filename, kfilename, PATH_MAX, &got);
	}


	while(i<MAX_PROCESS_OPEN_FILES)
	{												//find an empty slot and allocate it to this file that is going to be opened
		if (curproc->file_table[i] == NULL) {
			break;
		}
		i++;
	}


	if (i == MAX_PROCESS_OPEN_FILES) {
		kfree(kfilename);					/* in case the file table is full and there are no empty slots */
		return EMFILE;
	}
	if (total_file_opened>=MAX_SYSTEM_OPEN_FILES)
	{
		kfree(kfilename);
		return ENFILE;
	}

	if(open(kfilename, flags, i)){
		kfree(kfilename);
		return open(kfilename, flags, i);
	}

	
	total_file_opened++;
	*retVal = i;
	return 0;
}




/* read() call handler*/

int sys_read(int filehandler, userptr_t buf, size_t size, int *ret) {
	
	
	struct iovec iov;						
	struct uio myuio;
	off_t old_offset;
	struct File *file = curproc->file_table[filehandler];

	if(filehandler < 0 || filehandler >= MAX_PROCESS_OPEN_FILES) {				//check if the filename pointer is valid
		return EBADF;
	}

	if(curproc->file_table[filehandler]==0)
	{
		return EBADF;
	}
	

	int check = file->open_flags & O_ACCMODE;	
	if (check == O_WRONLY) {
		return EBADF;
	}
	lock_acquire(file->flock);				//Synchronizing the read operation
	old_offset = file->offset;

	uio_uinit(&iov, &myuio, buf, size, file->offset, UIO_READ);


	if (VOP_READ(file->v_ptr, &myuio)) {			//use VOP_READ to read from the file
		lock_release(file->flock);
		return VOP_READ(file->v_ptr, &myuio);
	}

	file->offset = myuio.uio_offset;
	*ret = file->offset - old_offset;
	lock_release(file->flock);				//releasing the lock
	return 0;
}



/* write() call handler*/
int sys_write(int filehandler, userptr_t buf, size_t size, int *ret) {
	struct iovec fiovec;
	struct uio fuio;
	int how;
	off_t old_offset;
	struct File *file = curproc->file_table[filehandler];

	if(filehandler < 0 || filehandler >= MAX_PROCESS_OPEN_FILES) {			//Check if the fd value is valid and that the file handle has been opened
		return EBADF;
	}

	if( !curproc->file_table[filehandler])
	{
		return EBADF;
	}
	
	how = file->open_flags & O_ACCMODE;				// Now, checking for "is file handle allowed to be written into?
	if (how == O_RDONLY) {
		return EBADF;
	}
	lock_acquire(file->flock);				//Synchronizing the write to the file
	old_offset = file->offset;
	uio_uinit(&fiovec, &fuio, buf, size, file->offset, UIO_WRITE);

	if (VOP_WRITE(file->v_ptr, &fuio)) {		//use VOP_WRITE to write into the file
		lock_release(file->flock);
		return VOP_WRITE(file->v_ptr, &fuio);
	}
	file->offset = fuio.uio_offset;
	*ret = file->offset - old_offset;
	lock_release(file->flock);						//Releasing Lock
	return 0;
}



/* dup2() call handler*/
int sys_dup2(int oldfd, int newfd) {

	struct File *fdesc;

	 
	if (oldfd >= MAX_PROCESS_OPEN_FILES || oldfd < 0 || newfd >= MAX_PROCESS_OPEN_FILES || newfd < 0 ){			//Check the validity of arguments
		return EBADF;
	}
	
	
	if (curproc->file_table[oldfd] == NULL) {		//checking if there is an oldfd that is already opened 
		return EBADF;
	}

	/* Check if newfd points to null*/
	if(curproc->file_table[newfd] != NULL){
		if(sys_close(newfd) !=0){
			return sys_close(newfd);      // Close existing fd
		}
	}

	
	fdesc = curproc->file_table[oldfd];
	lock_acquire(fdesc->flock);
	fdesc->references++;
	lock_release(fdesc->flock);
	return 0;
}




/* lseek() call handler */
int sys_lseek(int fd, off_t pos, userptr_t whence_ptr, off_t *ret64) {
	struct File *file;
	struct stat stats;
	int whence;
	int result;


	/*Check if the file descriptor passed in is valid */
	if(fd < 0 || fd > MAX_PROCESS_OPEN_FILES ){
		return EBADF;
	}

	if(!(file = curproc->file_table[fd]))
	{
		return EBADF;
	}

	if(VOP_ISSEEKABLE(file->v_ptr)==0){
		return ESPIPE;					// in case the SEEK fails
	}

	if(VOP_STAT(file->v_ptr, &stats)!=0){
		return VOP_STAT(file->v_ptr, &stats);
	}

	result = copyin(whence_ptr, &whence, sizeof(int));
	if(result !=0) {
		return result;
	}


	if(whence==SEEK_SET)
	{
		if(pos < 0){					// seek position is negative
			return EINVAL;
		}
		lock_acquire(file->flock);		// Adding locks to synchronize the whole process 
		*ret64 = file->offset = pos;
		lock_release(file->flock);
			
	}

	else if(whence==SEEK_CUR)
	{
		lock_acquire(file->flock);
		if(file->offset + pos < 0){
			lock_release(file->flock);			//Releasing Lock
			return EINVAL;
		}
		*ret64 = file->offset += pos;
		lock_release(file->flock);
	}

	else if(whence==SEEK_END)
	{
		if(stats.st_size + pos < 0){
				return EINVAL;
		}
		lock_acquire(file->flock);
		*ret64 = file->offset = stats.st_size + pos; 
		lock_release(file->flock);					//Release LOck
	}
	else 
		return EINVAL;

	return 0;
}






static int open(char *filename, int flags, int file_descriptor){
	struct File *file = kmalloc(sizeof(struct File*));

	struct vnode *vn;
	if(file==NULL){
		return ENFILE;
	}
 
	if (vfs_open(filename, flags, 0, &vn)) {
		kfree(file);
		return vfs_open(filename, flags, 0, &vn);
	}

	file->flock = lock_create("lock create");

	if(!lock_create("lock create")) {
		vfs_close(file->v_ptr);
		kfree(file);
		return ENFILE;
	}
	curproc->file_table[file_descriptor] = file;

	file->open_flags = flags;
	file->offset = 0;
	

	file->references = 1;
	file->v_ptr=vn;
	

	return 0;
}



/* close() system call handler */
int sys_close(int filehandler) {
	
	if(filehandler < 0 || filehandler >= MAX_PROCESS_OPEN_FILES) {		//check if fd is a valid file handle first
		return EBADF;
	}

	if(!curproc->file_table[filehandler])			//to avoid fd's whose entries are already NULL
		return EBADF;

	struct File *file = curproc->file_table[filehandler];

	lock_acquire(file->flock);

	curproc->file_table[filehandler] = NULL;
	file->references --;							//check if file handle at that position has an ref_count of 1

	if(file->references<=0) {
		lock_release(file->flock);
		vfs_close(file->v_ptr);
		lock_destroy(file->flock);
		kfree(file);
	}
	else{
		lock_release(file->flock);
	}
	total_file_opened--;
	return 0;							//on success , return 0

}
