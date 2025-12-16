try:
    import os
    import tensorflow as tf
    print("Package tensorflow is installed")

    gpu_num = 0 # Use "" to use the CPU
    os.environ["CUDA_VISIBLE_DEVICES"] = f"{gpu_num}"
    os.environ['TF_CPP_MIN_LOG_LEVEL'] = '3'

    gpus = tf.config.list_physical_devices('GPU')
    if gpus:
        try:
            tf.config.experimental.set_memory_growth(gpus[0], True)
        except RuntimeError as e:
            print(e)
    tf.get_logger().setLevel('ERROR')

    print("gpus_num:", gpu_num)
    print("gpus:", gpus)

except ImportError:
    print("Package tensorflow is not installed")